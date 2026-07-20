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

    std::suspend_always final_suspend() noexcept { return {}; }

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

class CoroReadWriteAwaiter {
public:
  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) noexcept { handle.resume(); }

  int await_resume() const noexcept { return 42; }
};

struct CoroReadWriteTask {
  struct promise_type {
    CoroReadWriteTask get_return_object() {
      return CoroReadWriteTask(std::coroutine_handle<promise_type>::from_promise(*this));
    }

    std::suspend_never initial_suspend() noexcept { return {}; }

    std::suspend_always final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    void unhandled_exception() {}
  };

  explicit CoroReadWriteTask(std::coroutine_handle<promise_type> h) : handle_(h) {}

  CoroReadWriteTask(CoroReadWriteTask&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

  ~CoroReadWriteTask() {
    if (handle_)
      handle_.destroy();
  }

private:
  std::coroutine_handle<promise_type> handle_;
};

CoroReadWriteTask bench_await_readwrite() {
  auto r = co_await CoroReadWriteAwaiter{};
  auto w = co_await CoroReadWriteAwaiter{};
  benchmark::DoNotOptimize(r);
  benchmark::DoNotOptimize(w);
}

static void BM_Coro_AwaitReadWrite(benchmark::State& state) {
  for (auto _ : state) {
    auto task = bench_await_readwrite();
    benchmark::DoNotOptimize(task);
  }
}

BENCHMARK(BM_Coro_AwaitReadWrite);

struct CoroSwitchAwaiter {
  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle) noexcept { handle.resume(); }

  int await_resume() const noexcept { return 0; }
};

struct CoroSwitchTask {
  struct promise_type {
    CoroSwitchTask get_return_object() {
      return CoroSwitchTask(std::coroutine_handle<promise_type>::from_promise(*this));
    }

    std::suspend_never initial_suspend() noexcept { return {}; }

    std::suspend_always final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    void unhandled_exception() {}
  };

  explicit CoroSwitchTask(std::coroutine_handle<promise_type> h) : handle_(h) {}

  CoroSwitchTask(CoroSwitchTask&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

  ~CoroSwitchTask() {
    if (handle_)
      handle_.destroy();
  }

private:
  std::coroutine_handle<promise_type> handle_;
};

CoroSwitchTask bench_switch_impl(int depth) {
  for (int i = 0; i < depth; ++i) {
    co_await CoroSwitchAwaiter{};
  }
}

static void BM_Coro_SwitchingOverhead(benchmark::State& state) {
  int depth = state.range(0);
  for (auto _ : state) {
    auto task = bench_switch_impl(depth);
    benchmark::DoNotOptimize(task);
  }
}

BENCHMARK(BM_Coro_SwitchingOverhead)->Arg(10)->Arg(100)->Arg(1000);

BENCHMARK_MAIN();
