#include "http_server.h"

#include "file_system.h"
#include "logger.h"
#include "thread_pool.h"
#include "websocket.h"
#include "ws_connection.h"

#include <openssl/evp.h>

#include <memory>
#include <string>
#include <utility>

namespace hps {

HttpServer::HttpServer(const TcpServer::Config& config) : server_(config) {}

HttpServer::~HttpServer() = default;

bool HttpServer::init() {
  server_.set_handler([this](std::shared_ptr<Connection> conn) {
    auto cm = get_conn_mutex(conn.get());
    std::lock_guard<std::mutex> cl(*cm);
    handle_connection(*conn);
  });
  return server_.init();
}

void HttpServer::start() {
  server_.start();
}

void HttpServer::stop() {
  server_.stop();
}

void HttpServer::get(std::string_view path, Handler handler) {
  router_.add(HttpMethod::GET, path, std::move(handler));
}

void HttpServer::post(std::string_view path, Handler handler) {
  router_.add(HttpMethod::POST, path, std::move(handler));
}

void HttpServer::put(std::string_view path, Handler handler) {
  router_.add(HttpMethod::PUT, path, std::move(handler));
}

void HttpServer::del(std::string_view path, Handler handler) {
  router_.add(HttpMethod::DELETE, path, std::move(handler));
}

void HttpServer::ws(std::string_view path, WsHandler handler) {
  ws_handlers_[std::string(path)] = std::move(handler);
}

void HttpServer::upload(std::string_view path, UploadHandler handler, UploadStreamSetup setup) {
  upload_handlers_[std::string(path)] = std::move(handler);
  if (setup) {
    upload_setups_[std::string(path)] = std::move(setup);
  }
  // 注册 POST 路由确保请求能通过路由器匹配，进入 upload handler 调用路径
  router_.add(HttpMethod::POST, path, [](const HttpRequest&, HttpResponse&) {});
}

std::shared_ptr<std::mutex> HttpServer::get_conn_mutex(Connection* c) {
  std::lock_guard<std::mutex> lock(conn_map_mutex_);
  auto& m = conn_mutexes_[c];
  if (!m) {
    m = std::make_shared<std::mutex>();
  }
  return m;
}

bool HttpServer::try_handle_ws_upgrade(Connection& conn, const HttpRequest& req, std::size_t total_consumed) {
  auto ws_it = ws_handlers_.find(req.path);
  if (ws_it == ws_handlers_.end()) {
    return false;
  }

  HttpResponse ws_resp;
  if (!ws_server_handshake(req, ws_resp)) {
    return false;
  }

  {
    auto serialized = ws_resp.serialize();
    std::lock_guard<std::mutex> wlock(conn.write_mutex());
    conn.write_buffer().append(serialized);
    conn.write_to_fd_locked();
  }
  conn.consume_read_buffer(total_consumed);

  auto conn_ptr = conn.shared_from_this();
  auto ws_conn = std::make_shared<WsConnection>(conn_ptr, [](WsFrame) {}, [](uint16_t) {});

  WsHandler ws_handler = ws_it->second;
  ws_handler(req, ws_conn);
  ws_conn->start_event_loop();
  return true;
}

void HttpServer::on_headers_done(HttpParser& parser, const HttpRequest& req, std::shared_ptr<UploadStreamContext> ctx) {
  if (upload_handlers_.find(req.path) == upload_handlers_.end()) {
    return;
  }

  // 先解析 Content-Disposition，确保 handler（非流式路径）能拿到 file_name
  auto cd_it = req.headers.find("Content-Disposition");
  if (cd_it != req.headers.end()) {
    auto fpos = cd_it->second.find("filename=\"");
    if (fpos != std::string::npos) {
      auto start = fpos + 10;
      auto end = cd_it->second.find('"', start);
      if (end != std::string::npos) {
        ctx->file_name = cd_it->second.substr(start, end - start);
      }
    }
  }
  if (ctx->file_name.empty()) {
    ctx->file_name = "unnamed";
  }

  // 鉴权：未登录或权限不足时不开流式，body 走内存缓冲，handler 返回 401
  if (auth_service_) {
    auto auth_it = req.headers.find("Authorization");
    bool authorized = false;
    if (auth_it != req.headers.end()) {
      const auto& header = auth_it->second;
      constexpr std::string_view kBearer = "Bearer ";
      if (header.size() > kBearer.size() && header.substr(0, kBearer.size()) == kBearer) {
        auto token = header.substr(kBearer.size());
        auto auth_user = auth_service_->validate_token(token);
        authorized = (auth_user.role >= UserRole::NORMAL);
      }
    }
    if (!authorized) {
      return;
    }
  }

  auto cl_it = req.headers.find("Content-Length");
  if (cl_it != req.headers.end()) {
    ctx->total_size = std::stoull(cl_it->second);
  }

  auto* md_ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(md_ctx, EVP_sha256(), nullptr);
  ctx->hash_ctx = md_ctx;

  parser.set_streaming_mode(true);
  parser.set_chunk_handler([ctx](std::string_view chunk) -> bool {
    auto* hctx = static_cast<EVP_MD_CTX*>(ctx->hash_ctx);
    EVP_DigestUpdate(hctx, chunk.data(), chunk.size());

    auto chunk_hash = FileSystem::sha256_hex(chunk.data(), chunk.size());

    if (ctx->store_chunk_data) {
      if (!ctx->store_chunk_data(chunk, chunk_hash)) {
        ctx->failed = true;
        return false;
      }
    }

    FileChunkRecord rec;
    rec.chunk_index = static_cast<int>(ctx->chunks.size());
    rec.chunk_hash = std::move(chunk_hash);
    rec.chunk_offset = ctx->current_offset;
    rec.chunk_size = static_cast<int>(chunk.size());
    ctx->current_offset += chunk.size();
    ctx->chunks.push_back(std::move(rec));
    return true;
  });

  auto setup_it = upload_setups_.find(req.path);
  if (setup_it != upload_setups_.end()) {
    setup_it->second(req, *ctx, parser);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void HttpServer::handle_connection(Connection& conn) {
  HttpParser* parser_ptr = nullptr;
  {
    std::lock_guard lock(parsers_mutex_);
    if (conn.state() == Connection::State::CLOSED) {
      parsers_.erase(&conn);
      return;
    }
    auto it = parsers_.find(&conn);
    if (it == parsers_.end()) {
      it = parsers_.emplace(&conn, HttpParser()).first;
    }
    parser_ptr = &it->second;
  }
  auto& parser = *parser_ptr;
  std::size_t total_consumed = 0;

  std::string buf;
  {
    std::lock_guard<std::mutex> rlock(conn.read_mutex());
    buf = conn.read_buffer();
  }

  while (total_consumed < buf.size()) {
    auto upload_ctx = std::make_shared<UploadStreamContext>();

    parser.set_headers_done_callback(
      [this, &parser, upload_ctx](const HttpRequest& req) { on_headers_done(parser, req, upload_ctx); });

    auto remaining = std::string_view(buf.data() + total_consumed, buf.size() - total_consumed);
    auto result = parser.feed(remaining);
    total_consumed += result.consumed;

    if (result.err == ParserError::INCOMPLETE) {
      break;
    }

    if (result.err == ParserError::BAD_REQUEST) {
      if (upload_ctx->failed) {
        send_error(conn, 400, "Bad Request", "上传失败");
      } else {
        send_error(conn, 400, "Bad Request", "请求格式错误");
      }
      conn.consume_read_buffer(total_consumed);
      {
        std::lock_guard lock(parsers_mutex_);
        parsers_.erase(&conn);
      }
      return;
    }

    if (result.err == ParserError::PAYLOAD_TOO_LARGE) {
      send_error(conn, 413, "Payload Too Large", "请求体过大");
      conn.consume_read_buffer(total_consumed);
      {
        std::lock_guard lock(parsers_mutex_);
        parsers_.erase(&conn);
      }
      return;
    }

    if (parser.state() != ParserState::COMPLETE) {
      break;
    }

    const auto& req = parser.request();

    auto up_it = req.headers.find("Upgrade");
    if (up_it != req.headers.end() && up_it->second == "websocket") {
      bool upgraded = try_handle_ws_upgrade(conn, req, total_consumed);
      if (!upgraded) {
        send_error(conn, 400, "Bad Request", "WebSocket 握手失败");
        conn.consume_read_buffer(total_consumed);
      }
      return;
    }

    Router::Handler handler;
    std::unordered_map<std::string, std::string> params;
    bool matched = router_.match(req.method, req.path, handler, params);

    if (!matched) {
      if (router_.path_exists(req.path)) {
        send_error(conn, 405, "Method Not Allowed", req.path);
      } else {
        send_error(conn, 404, "Not Found", req.path);
      }
      parser.reset();
      continue;
    }

    HttpRequest req_with_params = req;
    req_with_params.path_params = std::move(params);

    HttpResponse resp;

    if (!upload_ctx->chunks.empty() || !upload_ctx->file_name.empty()) {
      auto uit = upload_handlers_.find(req_with_params.path);
      if (uit != upload_handlers_.end()) {
        try {
          uit->second(req_with_params, *upload_ctx, resp);
        } catch (const std::exception& e) {
          Logger::_error("upload handler 异常: " + std::string(e.what()));
          send_error(conn, 500, "Internal Server Error", e.what());
          parser.reset();
          continue;
        } catch (...) {
          Logger::_error("upload handler 未知异常");
          send_error(conn, 500, "Internal Server Error", "unknown error");
          parser.reset();
          continue;
        }
        {
          auto serialized = resp.serialize();
          std::lock_guard<std::mutex> wlock(conn.write_mutex());
          conn.write_buffer().append(serialized);
        }
        parser.reset();
        continue;
      }
    }

    try {
      handler(req_with_params, resp);
    } catch (const std::exception& e) {
      Logger::_error("handler 异常: " + std::string(e.what()));
      send_error(conn, 500, "Internal Server Error", e.what());
      parser.reset();
      continue;
    } catch (...) {
      Logger::_error("handler 未知异常");
      send_error(conn, 500, "Internal Server Error", "unknown error");
      parser.reset();
      continue;
    }

    {
      auto serialized = resp.serialize();
      std::lock_guard<std::mutex> wlock(conn.write_mutex());
      conn.write_buffer().append(serialized);
    }

    parser.reset();
  }

  conn.consume_read_buffer(total_consumed);
}

void HttpServer::send_error(Connection& conn, int status, std::string_view text, std::string_view detail) {
  HttpResponse resp;
  resp.set_status(status, text);
  resp.set_content_type("text/plain; charset=utf-8");
  resp.body = std::to_string(status) + " " + std::string(text) + ": " + std::string(detail);
  resp.set_content_length(resp.body.size());

  auto serialized = resp.serialize();
  std::lock_guard<std::mutex> wlock(conn.write_mutex());
  conn.write_buffer().append(serialized);
}

} // namespace hps
