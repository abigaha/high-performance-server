#include "http_server.h"
#include "qps_runner.hpp"
#include "router.h"
#include "tcp_client.h"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
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
    if (!server->init()) {
      throw std::runtime_error("HTTP QPS 服务初始化失败");
    }
    port = server->actual_port();
    if (port == 0) {
      throw std::runtime_error("HTTP QPS 服务未获得监听端口");
    }
    server_thread = std::thread([this] { server->start(); });
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
    const std::string body = R"({"key":"value"})";
    return "POST /bench HTTP/1.1\r\n"
           "Host: localhost\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: " +
           std::to_string(body.size()) +
           "\r\n"
           "Connection: keep-alive\r\n"
           "\r\n" +
           body;
  }
};

thread_local std::unique_ptr<hps::TcpClient> g_http_client;

bool exchange_http_request(uint16_t port, const std::string& request, std::string_view expected_body) {
  if (!g_http_client || !g_http_client->is_connected()) {
    g_http_client = std::make_unique<hps::TcpClient>("127.0.0.1", port);
    if (!g_http_client->connect_to_server()) {
      g_http_client.reset();
      return false;
    }
  }
  if (!g_http_client->send_message(request)) {
    g_http_client.reset();
    return false;
  }

  std::string reply;
  if (!g_http_client->receive_message(reply, hps::ReadMode::RAW, 2000)) {
    g_http_client.reset();
    return false;
  }
  const auto header_end = reply.find("\r\n\r\n");
  return reply.starts_with("HTTP/1.1 200 OK\r\n") && header_end != std::string::npos &&
         std::string_view(reply).substr(header_end + 4) == expected_body;
}

int run_benchmark() {
  ServerFixture sfx;
  auto levels = hps::bench::default_qps_levels();

  const auto get_request = ServerFixture::make_get();
  hps::bench::run_qps_steps("HTTP GET", levels, [&sfx, &get_request](int) {
    return exchange_http_request(sfx.port, get_request, "OK");
  });

  const auto post_request = ServerFixture::make_post();
  hps::bench::run_qps_steps("HTTP POST", levels, [&sfx, &post_request](int) {
    return exchange_http_request(sfx.port, post_request, R"({"key":"value"})");
  });

  return EXIT_SUCCESS;
}

} // namespace

int main() noexcept {
  try {
    return run_benchmark();
  } catch (const std::exception& error) {
    std::cerr << "HTTP 服务 QPS 基准失败: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "HTTP 服务 QPS 基准失败: 未知异常\n";
  }
  return EXIT_FAILURE;
}
