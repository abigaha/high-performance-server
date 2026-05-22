#pragma once

#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

#include "logger.h"

namespace hps {

template <typename T>
class CoroItem {
public:
  class promise_type {
  public:
    auto get_return_object() {
      return CoroItem{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void unhandled_exception() { std::terminate(); }

    template <typename U>
    void return_value(U&& value) {
      return_value_ = std::make_shared<T>(std::forward<U>(value));
    }

    template <typename U>
    std::suspend_always yield_value(U&& value) {
      // 多次 co_yield 会覆盖为最新值（这是“当前值”语义）
      yield_value_ = std::make_shared<T>(std::forward<U>(value));
      return {};
    }

    uint64_t cid = hps::Logger::allocCoroutineId();
    std::optional<std::shared_ptr<T>> yield_value_;
    std::optional<std::shared_ptr<T>> return_value_;
  };

public:
  explicit CoroItem(std::coroutine_handle<promise_type> handle) : handle_(handle) {
    hps::Logger::CoroutineScope scope(handle_.promise().cid);
  }

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

  void resume() {
    if (!handle_ || handle_.done())
      return;

    // 清空旧 yield，避免读到上一次的值
    handle_.promise().yield_value_.reset();

    handle_.resume();
  }

  bool has_yield_value() const {
    return handle_ && handle_.promise().yield_value_.has_value() &&
           static_cast<bool>(handle_.promise().yield_value_.value());
  }

  // 读取但不清空（如果你需要）
  std::shared_ptr<T> yield_value() const {
    if (!handle_) {
      hps::Logger::getInstance().error("Invalid coroutine handle");
      throw std::runtime_error("Invalid coroutine handle");
    }
    if (!has_yield_value()) {
      hps::Logger::getInstance().error("No yield value set");
      throw std::runtime_error("No yield value set in coroutine");
    }
    return handle_.promise().yield_value_.value();
  }

  // 读取并清空（更像“消费”）
  std::shared_ptr<T> take_yield_value() {
    if (!handle_) {
      hps::Logger::getInstance().error("Invalid coroutine handle");
      throw std::runtime_error("Invalid coroutine handle");
    }

    auto& opt = handle_.promise().yield_value_;
    if (!opt.has_value() || !opt.value()) {
      hps::Logger::getInstance().error("No yield value set");
      throw std::runtime_error("No yield value set in coroutine");
    }

    // 关键：把 shared_ptr 从 optional 里“偷走”
    std::shared_ptr<T> out = std::move(*opt);
    opt.reset();
    return out;
  }

  bool has_return_value() const {
    return handle_ && handle_.promise().return_value_.has_value() &&
           static_cast<bool>(handle_.promise().return_value_.value());
  }

  std::shared_ptr<T> return_value() const {
    if (!handle_) {
      hps::Logger::getInstance().error("Invalid coroutine handle");
      throw std::runtime_error("Invalid coroutine handle");
    }
    if (!has_return_value()) {
      hps::Logger::getInstance().error("No return value set");
      throw std::runtime_error("No return value set in coroutine");
    }
    return handle_.promise().return_value_.value();
  }

private:
  std::coroutine_handle<promise_type> handle_{};
};

}  // namespace hps
