#include "logger.h"

#include <benchmark/benchmark.h>

#include <string>
#include <thread>
#include <vector>

namespace {

class LoggerBench : public benchmark::Fixture {
public:
  void SetUp(const ::benchmark::State& /*state*/) override { hps::Logger::init("bench"); }

  void TearDown(const ::benchmark::State& /*state*/) override { hps::Logger::shutdown(); }
};

BENCHMARK_DEFINE_F(LoggerBench, Info)(benchmark::State& state) {
  int count = state.range(0);
  for (auto _ : state) {
    for (int i = 0; i < count; ++i) {
      hps::Logger::_info("benchmark log message");
    }
  }
  state.SetItemsProcessed(state.iterations() * count);
}

BENCHMARK_REGISTER_F(LoggerBench, Info)->Arg(10000);

BENCHMARK_DEFINE_F(LoggerBench, Error)(benchmark::State& state) {
  int count = state.range(0);
  for (auto _ : state) {
    for (int i = 0; i < count; ++i) {
      hps::Logger::_error("benchmark error message");
    }
  }
  state.SetItemsProcessed(state.iterations() * count);
}

BENCHMARK_REGISTER_F(LoggerBench, Error)->Arg(10000);

BENCHMARK_DEFINE_F(LoggerBench, Multithreaded)(benchmark::State& state) {
  int count = state.range(0);
  int threads = static_cast<int>(state.range(1));
  for (auto _ : state) {
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) {
      workers.emplace_back([count] {
        for (int i = 0; i < count; ++i) {
          hps::Logger::_info("thread log message");
        }
      });
    }
    for (auto& t : workers) t.join();
  }
  state.SetItemsProcessed(state.iterations() * count * threads);
}

BENCHMARK_REGISTER_F(LoggerBench, Multithreaded)->Args({1000, 4});

BENCHMARK_DEFINE_F(LoggerBench, Format)(benchmark::State& state) {
  int count = state.range(0);
  for (auto _ : state) {
    for (int i = 0; i < count; ++i) {
      auto msg = "user_id=" + std::to_string(i) + " action=login ip=192.168.1." + std::to_string(i % 255);
      hps::Logger::_info(msg);
    }
  }
  state.SetItemsProcessed(state.iterations() * count);
}

BENCHMARK_REGISTER_F(LoggerBench, Format)->Arg(10000);

} // namespace

BENCHMARK_MAIN();
