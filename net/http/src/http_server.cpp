#include "http_server.h"

#include "logger.h"
#include "thread_pool.h"
#include "websocket.h"
#include "ws_connection.h"

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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): 连接处理器，多种协议路径 + 分片状态管理，多分支嵌套是固有复杂度
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
  const auto& buf = conn.read_buffer();
  std::size_t total_consumed = 0;

  while (total_consumed < buf.size()) {
    auto remaining = std::string_view(buf.data() + total_consumed, buf.size() - total_consumed);
    auto result = parser.feed(remaining);
    total_consumed += result.consumed;

    if (result.err == ParserError::INCOMPLETE) {
      break;
    }

    if (result.err == ParserError::BAD_REQUEST) {
      send_error(conn, 400, "Bad Request", "请求格式错误");
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
