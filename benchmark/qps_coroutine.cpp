#include "qps_runner.hpp"

#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>

namespace {

class CoroBench {
public:
  class Awaiter {
  public:
    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) noexcept { handle.resume(); }

    int await_resume() const noexcept { return 42; }
  };

  struct Promise {
    CoroBench get_return_object() { return CoroBench(std::coroutine_handle<Promise>::from_promise(*this)); }

    std::suspend_never initial_suspend() noexcept { return {}; }

    std::suspend_always final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    void unhandled_exception() {}
  };

  using promise_type = Promise;

  explicit CoroBench(std::coroutine_handle<Promise> h) : handle_(h) {}

  CoroBench(CoroBench&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

  ~CoroBench() {
    if (handle_)
      handle_.destroy();
  }

private:
  std::coroutine_handle<Promise> handle_;
};

CoroBench bench_coro_impl() {
  co_await CoroBench::Awaiter{};
}

} // namespace

int main() noexcept {
  try {
    auto levels = hps::bench::default_qps_levels();

    hps::bench::run_qps_steps("Coroutine Create+Run", levels, [](int) {
      auto c = bench_coro_impl();
      (void)c;
    });

    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "协程 QPS 基准失败: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "协程 QPS 基准失败: 未知异常\n";
  }
  return EXIT_FAILURE;
}
