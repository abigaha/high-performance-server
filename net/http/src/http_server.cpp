#include "http_server.h"

#include "logger.h"
#include "thread_pool.h"

#include <string>
#include <utility>

namespace hps {

HttpServer::HttpServer(const TcpServer::Config& config) : server_(config) {}
HttpServer::~HttpServer() = default; // out-of-line 定义，此 TU 已 include thread_pool.h

bool HttpServer::init() {
  server_.set_handler([this](std::shared_ptr<Connection> conn) {
    // 获取连接级 mutex，串行化同一 conn 的并发 handler 调用
    auto cm = get_conn_mutex(conn.get());
    std::lock_guard<std::mutex> cl(*cm);
    handle_connection(*conn);
  });
  return server_.init();
}

void HttpServer::start() { server_.start(); }

void HttpServer::stop() { server_.stop(); }

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

std::shared_ptr<std::mutex> HttpServer::get_conn_mutex(Connection* c) {
  std::lock_guard<std::mutex> lock(conn_map_mutex_);
  auto& m = conn_mutexes_[c];
  if (!m) {
    m = std::make_shared<std::mutex>();
  }
  return m;
}

void HttpServer::handle_connection(Connection& conn) {
  // 局部 parser，无共享状态竞态；每次从 read_buffer 起始解析
  HttpParser parser;
  const auto& buf = conn.read_buffer();
  std::size_t total_consumed = 0;

  while (total_consumed < buf.size()) {
    auto remaining = std::string_view(buf.data() + total_consumed, buf.size() - total_consumed);
    auto result = parser.feed(remaining);
    total_consumed += result.consumed;

    if (result.err == ParserError::INCOMPLETE) {
      break; // 等待更多数据
    }

    if (result.err == ParserError::BAD_REQUEST) {
      send_error(conn, 400, "Bad Request", "请求格式错误");
      conn.consume_read_buffer(total_consumed);
      return;
    }

    if (result.err == ParserError::PAYLOAD_TOO_LARGE) {
      send_error(conn, 413, "Payload Too Large", "请求体过大");
      conn.consume_read_buffer(total_consumed);
      return;
    }

    // result.err == OK 且解析完成
    if (parser.state() == ParserState::COMPLETE) {
      const auto& req = parser.request();

      // 路由匹配
      Router::Handler handler;
      std::unordered_map<std::string, std::string> params;
      bool matched = router_.match(req.method, req.path, handler, params);

      if (!matched) {
        send_error(conn, 404, "Not Found", req.path);
        parser.reset();
        continue;
      }

      // 构造带路径参数的 request 副本传入 handler
      HttpRequest req_with_params = req;
      req_with_params.path_params = std::move(params);

      HttpResponse resp;
      // 执行 handler（捕获异常防线程退出）
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

      // 序列化响应并写入 write_buffer（持 write_mutex）
      {
        auto serialized = resp.serialize();
        std::lock_guard<std::mutex> wlock(conn.write_mutex());
        conn.write_buffer().append(serialized);
      }

      parser.reset();
      continue; // 继续解析后续请求（keep-alive / pipeline）
    }

    break;
  }

  conn.consume_read_buffer(total_consumed);
}

void HttpServer::send_error(Connection& conn, int status, std::string_view text,
                            std::string_view detail) {
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
