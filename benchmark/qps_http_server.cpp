#include "http_server.h"
#include "qps_runner.hpp"
#include "router.h"
#include "tcp_client.h"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ServerFixture {
  std::unique_ptr<hps::HttpServer> server;
  std::thread server_thread;
  uint16_t port{0};

  ServerFixture() {
    hps::TcpServer::Config cfg;
    cfg.port = 0;
    cfg.thread_count = 2;
    server = std::make_unique<hps::HttpServer>(cfg);
    server->get("/bench", [](const hps::HttpRequest&, hps::HttpResponse& resp) {
      resp.set_status(200, "OK");
      resp.set_header("Content-Type", "text/plain");
      resp.body = "OK";
    });
    server->post("/bench", [](const hps::HttpRequest& req, hps::HttpResponse& resp) {
      resp.set_status(200, "OK");
      resp.set_header("Content-Type", "text/plain");
      resp.body = req.body;
    });
    server->init();
    server_thread = std::thread([this] { server->start(); });
    while (server->actual_port() == 0) {
      std::this_thread::yield();
    }
    port = server->actual_port();
  }

  ~ServerFixture() {
    server->stop();
    if (server_thread.joinable())
      server_thread.join();
  }

  static std::string make_get() {
    return "GET /bench HTTP/1.1\r\n"
           "Host: localhost\r\n"
           "Connection: keep-alive\r\n"
           "\r\n";
  }

  static std::string make_post() {
    return "POST /bench HTTP/1.1\r\n"
           "Host: localhost\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: 17\r\n"
           "Connection: keep-alive\r\n"
           "\r\n"
           "{\"key\":\"value\"}";
  }
};

thread_local hps::TcpClient* tls_client = nullptr;

} // namespace

int main() {
  ServerFixture sfx;
  auto levels = hps::bench::default_qps_levels();

  hps::bench::run_qps_steps("HTTP GET", levels, [&sfx](int) {
    if (tls_client == nullptr || !tls_client->is_connected()) {
      delete tls_client;
      tls_client = new hps::TcpClient("127.0.0.1", sfx.port);
      tls_client->connect_to_server();
    }
    auto req = ServerFixture::make_get();
    tls_client->send_message(req);
    std::string reply;
    tls_client->receive_message(reply, hps::ReadMode::RAW, 2000);
  });

  if (tls_client != nullptr) {
    tls_client->disconnect();
    delete tls_client;
    tls_client = nullptr;
  }

  hps::bench::run_qps_steps("HTTP POST", levels, [&sfx](int) {
    if (tls_client == nullptr || !tls_client->is_connected()) {
      delete tls_client;
      tls_client = new hps::TcpClient("127.0.0.1", sfx.port);
      tls_client->connect_to_server();
    }
    auto req = ServerFixture::make_post();
    tls_client->send_message(req);
    std::string reply;
    tls_client->receive_message(reply, hps::ReadMode::RAW, 2000);
  });

  if (tls_client != nullptr) {
    tls_client->disconnect();
    delete tls_client;
    tls_client = nullptr;
  }

  return 0;
}
