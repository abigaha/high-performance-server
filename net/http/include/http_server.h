#pragma once

#include "http_parser.h"
#include "http_request.h"
#include "http_response.h"
#include "i_http_server.h"
#include "router.h"
#include "tcp_server.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace hps {

class WsConnection;

class HttpServer : public IHttpServer {
public:
  using Handler = IRouter::Handler;
  using WsHandler = IHttpServer::WsHandler;

  explicit HttpServer(const TcpServer::Config& config = {});
  ~HttpServer() override;

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  bool init() override;
  void start() override;
  void stop() override;

  void get(std::string_view path, Handler handler) override;
  void post(std::string_view path, Handler handler) override;
  void put(std::string_view path, Handler handler) override;
  void del(std::string_view path, Handler handler) override;

  void ws(std::string_view path, WsHandler handler) override;

  uint16_t actual_port() const override { return server_.actual_port(); }

private:
  void handle_connection(Connection& conn);
  bool try_handle_ws_upgrade(Connection& conn, const HttpRequest& req, std::size_t total_consumed);
  static void send_error(Connection& conn, int status, std::string_view text, std::string_view detail);
  std::shared_ptr<std::mutex> get_conn_mutex(Connection* c);

  TcpServer server_;
  Router router_;
  std::unordered_map<std::string, WsHandler> ws_handlers_;

  std::mutex conn_map_mutex_;
  std::unordered_map<Connection*, std::shared_ptr<std::mutex>> conn_mutexes_;
};

} // namespace hps
