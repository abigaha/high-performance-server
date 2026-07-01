#pragma once

#include "lock_free_queue.hpp"
#include "move_only_function.h"
#include "thread_pool_base.h"

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace hps {

/**
 * 基于无锁队列 + 条件变量的锁无关（lock-free）线程池
 *
 * 使用 LockFreeQueue<MoveOnlyFunction> 作为无锁任务队列（SBO 优化，
 * 避免 std::function 堆分配），配合 std::condition_variable 实现
 * 工作线程的休眠唤醒，避免忙等自旋。
 */
class LockFreeThreadPool : public ThreadPoolBase<LockFreeThreadPool> {
public:
  explicit LockFreeThreadPool(size_t numThreads);
  ~LockFreeThreadPool();

  /** CRTP 实现 */
  template <typename F, typename... Args>
    requires std::invocable<F, Args...>
  void enqueue_impl(F&& f, Args&&... args);

  void wait_impl();
  void stop_impl();

private:
  void worker(std::stop_token stop_token);

  std::vector<std::jthread> workers_;     ///< 工作线程集合（jthread 自动 join）
  LockFreeQueue<MoveOnlyFunction> tasks_; ///< 无锁任务队列（MoveOnlyFunction SBO 优化）
  std::mutex cv_mutex_;                   ///< 条件变量互斥量
  std::condition_variable cv_;            ///< 任务到达通知
  std::atomic<int> pending_{0};           ///< 待处理任务数
};

template <typename F, typename... Args>
  requires std::invocable<F, Args...>
void LockFreeThreadPool::enqueue_impl(F&& f, Args&&... args) {
  auto bound = [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::move(args)...); };
  bool ok = tasks_.push(MoveOnlyFunction(std::move(bound)));
  if (!ok) {
    return;
  }
  pending_.fetch_add(1, std::memory_order_release);
  cv_.notify_one();
}

} // namespace hps
