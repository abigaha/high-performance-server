#pragma once

#include "auth_service.h"
#include "http_parser.h"
#include "http_request.h"
#include "http_response.h"
#include "i_http_server.h"
#include "router.h"
#include "tcp_server.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace hps {

class WsConnection;

struct UploadStreamContext {
  using ChunkStoreFunc = std::function<bool(std::string_view data, const std::string& chunk_hash)>;

  std::string file_name;
  std::string file_hash;
  std::size_t total_size{0};
  std::size_t current_offset{0};
  std::vector<FileChunkRecord> chunks;
  void* hash_ctx{nullptr};
  ChunkStoreFunc store_chunk_data;
  bool failed{false};
  bool size_exceeded{false};
};

class HttpServer : public IHttpServer {
public:
  using Handler = IRouter::Handler;
  using WsHandler = IHttpServer::WsHandler;
  using UploadHandler = std::function<void(const HttpRequest&, UploadStreamContext&, HttpResponse&)>;
  using UploadStreamSetup = std::function<void(const HttpRequest&, UploadStreamContext&, HttpParser&)>;

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
  void upload(std::string_view path, UploadHandler handler, UploadStreamSetup setup = nullptr);

  uint16_t actual_port() const override { return server_.actual_port(); }

private:
  void handle_connection(Connection& conn);
  void on_headers_done(HttpParser& parser, const HttpRequest& req, std::shared_ptr<UploadStreamContext> ctx);
  bool try_handle_ws_upgrade(Connection& conn, const HttpRequest& req, std::size_t total_consumed);
  static void send_error(Connection& conn, int status, std::string_view text, std::string_view detail);
  std::shared_ptr<std::mutex> get_conn_mutex(Connection* c);

  TcpServer server_;
  Router router_;
  std::unordered_map<std::string, WsHandler> ws_handlers_;
  std::unordered_map<std::string, UploadHandler> upload_handlers_;
  std::unordered_map<std::string, UploadStreamSetup> upload_setups_;

  std::mutex conn_map_mutex_;
  std::unordered_map<Connection*, std::shared_ptr<std::mutex>> conn_mutexes_;

  std::mutex parsers_mutex_;
  std::unordered_map<Connection*, HttpParser> parsers_;
};

} // namespace hps
