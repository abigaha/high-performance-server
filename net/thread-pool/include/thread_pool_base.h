#pragma once

#include <concepts>
#include <stop_token>
#include <utility>

namespace hps {

/**
 * 线程池 CRTP 基类（静态多态）
 *
 * enqueue 是高频调用路径，使用 CRTP 消除虚函数开销。
 * 派生类需实现 enqueue_impl / wait_impl / stop_impl。
 *
 * @tparam Derived 派生类类型（如 LockFreeThreadPool / LockedThreadPool）
 */
template <typename Derived>
class ThreadPoolBase {
public:
  template <typename F, typename... Args>
    requires std::invocable<F, Args...>
  void enqueue(F&& f, Args&&... args) {
    static_cast<Derived*>(this)->enqueue_impl(std::forward<F>(f), std::forward<Args>(args)...);
  }

  void wait_for_all_tasks() { static_cast<Derived*>(this)->wait_impl(); }

  void stop() { static_cast<Derived*>(this)->stop_impl(); }

  ThreadPoolBase(const ThreadPoolBase&) = delete;
  ThreadPoolBase& operator=(const ThreadPoolBase&) = delete;

protected:
  ThreadPoolBase() = default;
  ~ThreadPoolBase() = default;
};

} // namespace hps
