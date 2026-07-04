#pragma once

#include "move_only_function.h"
#include "thread_pool_base.h"

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace hps {

class LockedThreadPool : public ThreadPoolBase<LockedThreadPool> {
public:
  explicit LockedThreadPool(std::size_t num_threads);
  ~LockedThreadPool();

  LockedThreadPool(const LockedThreadPool&) = delete;
  LockedThreadPool& operator=(const LockedThreadPool&) = delete;

  template <typename F, typename... Args>
    requires std::invocable<F, Args...>
  void enqueue_impl(F&& f, Args&&... args);

  void wait_impl();
  void stop_impl();

  bool wait_for(std::chrono::milliseconds timeout);

private:
  void worker(std::stop_token stop_token);

  std::vector<std::jthread> workers_;
  std::queue<MoveOnlyFunction> tasks_;
  std::mutex queue_mutex_;
  std::condition_variable cv_;
  std::atomic<int> pending_{0};
  bool stop_{false};
};

template <typename F, typename... Args>
  requires std::invocable<F, Args...>
void LockedThreadPool::enqueue_impl(F&& f, Args&&... args) {
  auto bound = [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::move(args)...); };
  {
    std::lock_guard lock(queue_mutex_);
    if (stop_) {
      return;
    }
    tasks_.emplace(std::move(bound));
  }
  pending_.fetch_add(1, std::memory_order_release);
  cv_.notify_one();
}

} // namespace hps
