#include "connection.h"
#include "tcp_client.h"
#include "tcp_server.h"

#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class TcpServerBench : public benchmark::Fixture {
public:
  void SetUp(const ::benchmark::State& /*state*/) override {
    config_.port = 0;
    config_.thread_count = 2;
    server_ = std::make_unique<hps::TcpServer>(config_);
    server_->set_handler([](std::shared_ptr<hps::Connection> conn) {
      std::string payload;
      {
        std::lock_guard read_lock(conn->read_mutex());
        payload = conn->read_buffer();
        conn->consume_read_buffer_locked(payload.size());
      }
      if (!payload.empty()) {
        std::lock_guard write_lock(conn->write_mutex());
        conn->write_buffer().append(payload);
      }
    });
    if (!server_->init()) {
      throw std::runtime_error("TCP 基准服务初始化失败");
    }
    port_ = server_->actual_port();
    if (port_ == 0) {
      throw std::runtime_error("TCP 基准服务未获得监听端口");
    }
    server_thread_ = std::thread([this] { server_->start(); });
  }

  void TearDown(const ::benchmark::State& /*state*/) override {
    server_->stop();
    if (server_thread_.joinable())
      server_thread_.join();
    server_.reset();
  }

  hps::TcpServer::Config config_;
  std::unique_ptr<hps::TcpServer> server_;
  std::thread server_thread_;
  uint16_t port_{0};
};

// cppcheck-suppress unknownMacro
BENCHMARK_DEFINE_F(TcpServerBench, Connect)(benchmark::State& state) {
  for (auto _ : state) {
    hps::TcpClient client("127.0.0.1", port_);
    auto ok = client.connect_to_server();
    if (ok)
      client.disconnect();
    benchmark::DoNotOptimize(ok);
  }
}

BENCHMARK_REGISTER_F(TcpServerBench, Connect);

BENCHMARK_DEFINE_F(TcpServerBench, SendReceive)(benchmark::State& state) {
  std::string payload(static_cast<std::size_t>(state.range(0)), 'x');
  for (auto _ : state) {
    hps::TcpClient client("127.0.0.1", port_);
    if (!client.connect_to_server())
      continue;
    auto sent = client.send_message(payload);
    std::string reply;
    auto received = client.receive_message(reply, hps::ReadMode::RAW, 2000);
    client.disconnect();
    benchmark::DoNotOptimize(sent);
    benchmark::DoNotOptimize(received);
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK_REGISTER_F(TcpServerBench, SendReceive)->Arg(64)->Arg(1024)->Arg(16384);

BENCHMARK_DEFINE_F(TcpServerBench, ConcurrentConnections)(benchmark::State& state) {
  int num_conns = state.range(0);
  for (auto _ : state) {
    std::vector<hps::TcpClient> clients;
    clients.reserve(static_cast<std::size_t>(num_conns));
    for (int i = 0; i < num_conns; ++i) {
      clients.emplace_back("127.0.0.1", port_);
    }
    for (auto& c : clients) {
      c.connect_to_server();
    }
    for (auto& c : clients) {
      c.disconnect();
    }
  }
  state.SetItemsProcessed(state.iterations() * num_conns);
}

BENCHMARK_REGISTER_F(TcpServerBench, ConcurrentConnections)->Arg(10)->Arg(50)->Arg(100);

BENCHMARK_DEFINE_F(TcpServerBench, Throughput)(benchmark::State& state) {
  int64_t bytes = state.range(0);
  std::string payload(static_cast<std::size_t>(bytes), 'x');
  int64_t total_bytes = 0;
  for (auto _ : state) {
    hps::TcpClient client("127.0.0.1", port_);
    if (!client.connect_to_server())
      continue;
    client.send_message(payload);
    std::string reply;
    client.receive_message(reply, hps::ReadMode::RAW, 5000);
    client.disconnect();
    total_bytes += static_cast<int64_t>(payload.size() + reply.size());
  }
  state.SetBytesProcessed(total_bytes);
}

BENCHMARK_REGISTER_F(TcpServerBench, Throughput)->Arg(4096)->Arg(65536)->Arg(262144);

} // namespace

BENCHMARK_MAIN();
