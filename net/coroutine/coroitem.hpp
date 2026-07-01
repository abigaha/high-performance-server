#pragma once

#include "logger.h"
#include "memory_pool_factory.h"

#include <algorithm>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace hps {

template <typename T>
class CoroItem {
public:
  class promise_type {
  public:
    auto get_return_object() { return CoroItem{std::coroutine_handle<promise_type>::from_promise(*this)}; }

    std::suspend_always initial_suspend() { return {}; }

    std::suspend_always final_suspend() noexcept { return {}; }

    void unhandled_exception() { exception_ = std::current_exception(); }

    std::stop_token get_stop_token() const { return stop_source_.get_token(); }

    bool request_stop() noexcept { return stop_source_.request_stop(); }

    template <typename U>
    void return_value(U&& value) {
      return_value_.emplace(std::forward<U>(value));
    }

    template <typename U>
    std::suspend_always yield_value(U&& value) {
      yield_value_.emplace(std::forward<U>(value));
      return {};
    }

    // NOLINTNEXTLINE(hicpp-new-delete-operators,misc-new-delete-overloads)
    void* operator new(size_t coroSize) { return get_pool().allocate(coroSize); }

    void operator delete(void* ptr, size_t coroSize) {
      auto& pool = get_pool();
      pool.deallocate(ptr, coroSize);
    }

    static MemoryPoolBase<TieredMemoryPool>& get_pool() {
      static auto instance = CreateMemoryPool();
      return *instance;
    }

    std::exception_ptr exception_;
    uint64_t cid_ = hps::Logger::allocCoroutineId();
    std::optional<T> yield_value_;
    std::optional<T> return_value_;
    std::stop_source stop_source_;
  };

public:
  explicit CoroItem(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

  CoroItem(const CoroItem&) = delete;
  CoroItem& operator=(const CoroItem&) = delete;

  CoroItem(CoroItem&& other) noexcept : handle_(other.handle_) { other.handle_ = {}; }

  CoroItem& operator=(CoroItem&& other) noexcept {
    if (this != &other) {
      if (handle_)
        handle_.destroy();
      handle_ = other.handle_;
      other.handle_ = {};
    }
    return *this;
  }

  ~CoroItem() {
    if (handle_)
      handle_.destroy();
  }

  bool done() const { return !handle_ || handle_.done(); }

  void cancel() {
    if (!handle_ || handle_.done())
      return;
    handle_.promise().request_stop();
  }

  void resume() {
    if (!handle_ || handle_.done())
      return;

    // 清空旧 yield，避免读到上一次的值
    handle_.promise().yield_value_.reset();

    hps::Logger::CoroutineScope scope(handle_.promise().cid_);
    handle_.resume();

    auto& exc = handle_.promise().exception_;
    if (exc) [[unlikely]] {
      auto e = std::move(exc);
      std::rethrow_exception(e);
    }
  }

  bool has_yield_value() const { return handle_ && handle_.promise().yield_value_.has_value(); }

  // 读取但不清空
  const T& yield_value() const {
    if (!handle_) {
      hps::Logger::getInstance().error("Invalid coroutine handle");
      throw std::runtime_error("Invalid coroutine handle");
    }
    if (!has_yield_value()) {
      hps::Logger::getInstance().error("No yield value set");
      throw std::runtime_error("No yield value set in coroutine");
    }
    return *handle_.promise().yield_value_;
  }

  // 读取并清空
  T take_yield_value() {
    if (!handle_) {
      hps::Logger::getInstance().error("Invalid coroutine handle");
      throw std::runtime_error("Invalid coroutine handle");
    }
    auto& opt = handle_.promise().yield_value_;
    if (!opt.has_value()) {
      hps::Logger::getInstance().error("No yield value set");
      throw std::runtime_error("No yield value set in coroutine");
    }

    T out = std::move(*opt);
    opt.reset();
    return out;
  }

  bool has_return_value() const { return handle_ && handle_.promise().return_value_.has_value(); }

  const T& return_value() const {
    if (!handle_) {
      hps::Logger::getInstance().error("Invalid coroutine handle");
      throw std::runtime_error("Invalid coroutine handle");
    }
    if (!has_return_value()) {
      hps::Logger::getInstance().error("No return value set");
      throw std::runtime_error("No return value set in coroutine");
    }
    return *handle_.promise().return_value_;
  }

private:
  std::coroutine_handle<promise_type> handle_{};
};

} // namespace hps
