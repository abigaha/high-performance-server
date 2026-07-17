#pragma once

#include "lock_free_queue.hpp"
#include "move_only_function.h"
#include "thread_pool_base.h"

#include <atomic>
#include <concepts>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace hps {

/**
 * 基于无锁队列的锁无关（lock-free）线程池
 *
 * 使用 LockFreeQueue<MoveOnlyFunction> 作为无锁任务队列（SBO 优化，
 * 避免 std::function 堆分配），worker 直接用队列原生的 pop() 阻塞获取任务，
 * 无需额外的条件变量和互斥量。
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

  std::vector<std::jthread> workers_;            ///< 工作线程集合（jthread 自动 join）
  LockFreeQueue<MoveOnlyFunction, 65535> tasks_; ///< 无锁任务队列（大容量降低 ABA 碰撞概率）
  std::atomic<int> pending_{0};                  ///< 待处理任务数
};

template <typename F, typename... Args>
  requires std::invocable<F, Args...>
void LockFreeThreadPool::enqueue_impl(F&& f, Args&&... args) {
  auto bound = [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::move(args)...); };
  if (tasks_.push(MoveOnlyFunction(std::move(bound)))) {
    pending_.fetch_add(1, std::memory_order_release);
  }
}

} // namespace hps
