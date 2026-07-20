#include "connection.h"
#include "qps_runner.hpp"
#include "tcp_client.h"
#include "tcp_server.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ServerFixture {
  hps::TcpServer::Config config;
  std::unique_ptr<hps::TcpServer> server;
  std::thread server_thread;
  uint16_t port{0};

  ServerFixture() {
    config.port = 0;
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
};

thread_local hps::TcpClient* tls_client = nullptr;

} // namespace

int main() {
  ServerFixture sfx;
  auto levels = hps::bench::default_qps_levels();

  hps::bench::run_qps_steps("TCP Connect", levels, [&sfx](int) {
    hps::TcpClient client("127.0.0.1", sfx.port);
    if (client.connect_to_server()) {
      client.disconnect();
    }
  });

  hps::bench::run_qps_steps("TCP Send/Receive 1KB", levels, [&sfx](int /*tid*/) {
    if (tls_client == nullptr || !tls_client->is_connected()) {
      delete tls_client;
      tls_client = new hps::TcpClient("127.0.0.1", sfx.port);
      tls_client->connect_to_server();
    }
    std::string payload(1024, 'x');
    tls_client->send_message(payload);
    std::string reply;
    tls_client->receive_message(reply, hps::ReadMode::RAW, 2000);
  });

  // Clean up thread-local clients after the test
  if (tls_client != nullptr) {
    tls_client->disconnect();
    delete tls_client;
    tls_client = nullptr;
  }

  return 0;
}
