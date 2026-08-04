#include "http_server.h"
#include "router.h"
#include "tcp_client.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

class HttpServerBench : public benchmark::Fixture {
public:
  void SetUp(const ::benchmark::State& /*state*/) override {
    hps::TcpServer::Config cfg;
    cfg.port = 0;
    cfg.thread_count = 2;
    server_ = std::make_unique<hps::HttpServer>(cfg);
    server_->get("/bench", [](const hps::HttpRequest&, hps::HttpResponse& resp) {
      resp.set_status(200, "OK");
      resp.set_header("Content-Type", "text/plain");
      resp.body = "OK";
    });
    server_->post("/bench", [](const hps::HttpRequest& req, hps::HttpResponse& resp) {
      resp.set_status(200, "OK");
      resp.set_header("Content-Type", "text/plain");
      resp.body = req.body;
    });
    if (!server_->init()) {
      std::cerr << "HTTP 基准服务初始化失败\n";
      std::exit(EXIT_FAILURE);
    }
    port_ = server_->actual_port();
    server_thread_ = std::thread([this] { server_->start(); });
  }

  void TearDown(const ::benchmark::State& /*state*/) override {
    server_->stop();
    if (server_thread_.joinable())
      server_thread_.join();
    server_.reset();
  }

  std::string make_get() {
    return "GET /bench HTTP/1.1\r\n"
           "Host: localhost\r\n"
           "Connection: keep-alive\r\n"
           "\r\n";
  }

  std::string make_post(const std::string& body) {
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

  std::string make_pipelined(int count) {
    std::string req;
    const auto* single = "GET /bench HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Connection: keep-alive\r\n"
                         "\r\n";
    for (int i = 0; i < count; ++i) {
      req += single;
    }
    return req;
  }

  std::unique_ptr<hps::HttpServer> server_;
  std::thread server_thread_;
  uint16_t port_{0};
};

BENCHMARK_DEFINE_F(HttpServerBench, HttpGet)(benchmark::State& state) {
  auto req = make_get();
  for (auto _ : state) {
    hps::TcpClient client("127.0.0.1", port_);
    if (!client.connect_to_server())
      continue;
    client.send_message(req);
    std::string reply;
    auto ok = client.receive_message(reply, hps::ReadMode::RAW, 2000);
    client.disconnect();
    benchmark::DoNotOptimize(ok);
  }
}

BENCHMARK_REGISTER_F(HttpServerBench, HttpGet);

BENCHMARK_DEFINE_F(HttpServerBench, HttpPost)(benchmark::State& state) {
  const auto* body = R"({"key":"value","count":42})";
  auto req = make_post(body);
  for (auto _ : state) {
    hps::TcpClient client("127.0.0.1", port_);
    if (!client.connect_to_server())
      continue;
    client.send_message(req);
    std::string reply;
    auto ok = client.receive_message(reply, hps::ReadMode::RAW, 2000);
    client.disconnect();
    benchmark::DoNotOptimize(ok);
  }
}

BENCHMARK_REGISTER_F(HttpServerBench, HttpPost);

BENCHMARK_DEFINE_F(HttpServerBench, HttpPostLargeBody)(benchmark::State& state) {
  std::string body(static_cast<std::size_t>(state.range(0)), 'x');
  auto req = make_post(body);
  for (auto _ : state) {
    hps::TcpClient client("127.0.0.1", port_);
    if (!client.connect_to_server())
      continue;
    client.send_message(req);
    std::string reply;
    auto ok = client.receive_message(reply, hps::ReadMode::RAW, 5000);
    client.disconnect();
    benchmark::DoNotOptimize(ok);
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK_REGISTER_F(HttpServerBench, HttpPostLargeBody)->Arg(1024)->Arg(65536);

BENCHMARK_DEFINE_F(HttpServerBench, HttpPipelined)(benchmark::State& state) {
  int count = state.range(0);
  auto req = make_pipelined(count);
  for (auto _ : state) {
    hps::TcpClient client("127.0.0.1", port_);
    if (!client.connect_to_server())
      continue;
    client.send_message(req);
    for (int i = 0; i < count; ++i) {
      std::string single_reply;
      auto ok = client.receive_message(single_reply, hps::ReadMode::RAW, 2000);
      benchmark::DoNotOptimize(ok);
    }
    client.disconnect();
  }
  state.SetItemsProcessed(state.iterations() * count);
}

BENCHMARK_REGISTER_F(HttpServerBench, HttpPipelined)->Arg(2)->Arg(5)->Arg(10);

} // namespace

BENCHMARK_MAIN();
