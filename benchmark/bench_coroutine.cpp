#include "connection.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

namespace {

class CoroutineBench {
public:
  class Awaiter {
  public:
    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) noexcept { handle.resume(); }

    int await_resume() const noexcept { return 42; }
  };

  struct Promise {
    CoroutineBench get_return_object() { return CoroutineBench(std::coroutine_handle<Promise>::from_promise(*this)); }

    std::suspend_never initial_suspend() noexcept { return {}; }

    std::suspend_never final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    void unhandled_exception() {}
  };

  using promise_type = Promise;

  explicit CoroutineBench(std::coroutine_handle<Promise> h) : handle_(h) {}

  CoroutineBench(CoroutineBench&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

  ~CoroutineBench() {
    if (handle_)
      handle_.destroy();
  }

private:
  std::coroutine_handle<Promise> handle_;
};

CoroutineBench bench_coro_impl() {
  co_await CoroutineBench::Awaiter{};
}

} // namespace

static void BM_Coroutine_CreateAndRun(benchmark::State& state) {
  for (auto _ : state) {
    auto c = bench_coro_impl();
    benchmark::DoNotOptimize(c);
  }
}

BENCHMARK(BM_Coroutine_CreateAndRun);

BENCHMARK_MAIN();
