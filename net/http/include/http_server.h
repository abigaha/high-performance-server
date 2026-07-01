#pragma once

#include "http_parser.h"
#include "http_request.h"
#include "http_response.h"
#include "i_http_server.h"
#include "router.h"
#include "tcp_server.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace hps {

class HttpServer : public IHttpServer {
public:
  using Handler = IRouter::Handler;

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

  uint16_t actual_port() const override { return server_.actual_port(); }

private:
  void handle_connection(Connection& conn);
  static void send_error(Connection& conn, int status, std::string_view text, std::string_view detail);
  std::shared_ptr<std::mutex> get_conn_mutex(Connection* c);

  TcpServer server_;
  Router router_;

  std::mutex conn_map_mutex_;
  std::unordered_map<Connection*, std::shared_ptr<std::mutex>> conn_mutexes_;
};

} // namespace hps
