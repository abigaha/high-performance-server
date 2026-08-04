#include "connection.h"
#include "qps_runner.hpp"
#include "tcp_client.h"
#include "tcp_server.h"

#include <atomic>
#include <chrono>
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

constexpr auto kServerStartTimeout = std::chrono::seconds{3};
constexpr auto kServerStartPollInterval = std::chrono::milliseconds{10};

struct ServerFixture {
  hps::TcpServer::Config config;
  std::unique_ptr<hps::TcpServer> server;
  std::thread server_thread;
  uint16_t port{0};

  ServerFixture() {
    config.port = 0;
    config.backlog = 4096;
    config.thread_count = 2;
    server = std::make_unique<hps::TcpServer>(config);
    server->set_handler([](std::shared_ptr<hps::Connection> conn) {
      conn->read_from_fd();
      auto& buf = conn->read_buffer();
      if (!buf.empty()) {
        conn->write_buffer() = buf;
        conn->consume_read_buffer(buf.size());
        conn->write_to_fd();
      }
    });
    if (!server->init()) {
      throw std::runtime_error("TCP QPS 服务初始化失败");
    }
    port = server->actual_port();
    if (port == 0) {
      throw std::runtime_error("TCP QPS 服务未获得监听端口");
    }
    server_thread = std::thread([this] { server->start(); });
    const auto deadline = std::chrono::steady_clock::now() + kServerStartTimeout;
    while (!server->is_running() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(kServerStartPollInterval);
    }
    if (!server->is_running()) {
      server->stop();
      if (server_thread.joinable()) {
        server_thread.join();
      }
      throw std::runtime_error("TCP QPS 服务启动超时");
    }
  }

  ~ServerFixture() {
    server->stop();
    if (server_thread.joinable())
      server_thread.join();
  }
};

thread_local std::unique_ptr<hps::TcpClient> g_tcp_client;

bool exchange_tcp_payload(uint16_t port, const std::string& payload) {
  constexpr int kMaxAttempts = 3;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (!g_tcp_client || !g_tcp_client->is_connected()) {
      g_tcp_client = std::make_unique<hps::TcpClient>("127.0.0.1", port);
      if (!g_tcp_client->connect_to_server()) {
        g_tcp_client.reset();
        continue;
      }
    }

    std::string reply;
    if (g_tcp_client->send_message(payload) && g_tcp_client->receive_message(reply, hps::ReadMode::RAW, 2000) &&
        reply == payload) {
      return true;
    }
    // A failed exchange may leave the stream half-consumed; reconnect before
    // retrying so a later sample cannot inherit stale bytes.
    g_tcp_client.reset();
  }
  return false;
}

} // namespace

int main() {
  try {
    ServerFixture sfx;
    auto levels = hps::bench::default_qps_levels();

    hps::bench::run_qps_steps("TCP Connect", levels, [&sfx](int) {
      hps::TcpClient client("127.0.0.1", sfx.port);
      return client.connect_to_server();
    });

    const std::string payload(1024, 'x');
    hps::bench::run_qps_steps("TCP Send/Receive 1KB", levels, [&sfx, &payload](int) {
      return exchange_tcp_payload(sfx.port, payload);
    });

    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "TCP QPS 测试异常：" << error.what() << '\n';
  } catch (...) {
    std::cerr << "TCP QPS 测试发生未知异常\n";
  }
  return EXIT_FAILURE;
}
