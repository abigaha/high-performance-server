#pragma once

#include "tcp_server.h"
#include "http_parser.h"
#include "http_request.h"
#include "http_response.h"
#include "router.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace hps {

/**
 * HTTP 服务器
 *
 * 封装 TcpServer + Router，提供 HTTP 请求路由分发。
 * 用户通过 get/post/put/del 注册路由，start() 启动服务。
 *
 * 连接处理：每次可读事件后，用局部 HttpParser 解析 read_buffer，
 * 解析完成后路由匹配并执行 handler，响应写回 write_buffer。
 * 同一连接的处理器通过 conn 级 mutex 串行化，避免并发竞态。
 */
class HttpServer {
public:
  using Handler = Router::Handler;

  explicit HttpServer(const TcpServer::Config& config = {});
  ~HttpServer(); // out-of-line（持有 unique_ptr<ThreadPool> 的 TcpServer 需完整类型）

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  /** 初始化底层 TcpServer（socket/bind/listen/epoll） */
  bool init();

  /** 启动事件循环（阻塞，由主线程调用） */
  void start();

  /** 停止服务器 */
  void stop();

  /** 便捷注册：GET */
  void get(std::string_view path, Handler handler);
  /** 便捷注册：POST */
  void post(std::string_view path, Handler handler);
  /** 便捷注册：PUT */
  void put(std::string_view path, Handler handler);
  /** 便捷注册：DELETE */
  void del(std::string_view path, Handler handler);

  /** 实际绑定端口（port=0 自动分配时有效） */
  uint16_t actual_port() const { return server_.actual_port(); }

private:
  /** 连接处理器：解析请求 + 路由分发 */
  void handle_connection(Connection& conn);

  /** 发送错误响应（默认响应体） */
  static void send_error(Connection& conn, int status, std::string_view text,
                         std::string_view detail);

  /** 获取/创建连接级 mutex（串行化同一 conn 的 handler） */
  std::shared_ptr<std::mutex> get_conn_mutex(Connection* c);

  TcpServer server_;
  Router router_;

  std::mutex conn_map_mutex_;
  std::unordered_map<Connection*, std::shared_ptr<std::mutex>> conn_mutexes_;
};

} // namespace hps
