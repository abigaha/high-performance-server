#pragma once

#include "lock_free_queue.hpp"
#include "move_only_function.h"
#include "thread_pool_base.h"

#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace hps {

/**
 * 基于无锁队列的锁无关（lock-free）线程池
 *
 * 使用 LockFreeQueue<MoveOnlyFunction> 作为无锁任务队列（SBO 优化，
 * 避免 std::function 堆分配）。条件变量只负责工作可用性和生命周期同步，
 * 因此空闲 worker 不会轮询无锁队列。
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
  void finish_enqueue(bool task_enqueued);

  std::vector<std::jthread> workers_;            ///< 工作线程集合（jthread 自动 join）
  LockFreeQueue<MoveOnlyFunction, 65535> tasks_; ///< 无锁任务队列（大容量降低 ABA 碰撞概率）
  std::mutex state_mutex_;                       ///< 任务计数和生命周期状态保护锁
  std::mutex stop_mutex_;                        ///< 串行化 stop 调用
  std::condition_variable work_cv_;              ///< worker 空闲等待
  std::condition_variable idle_cv_;              ///< wait_for_all_tasks 等待
  std::condition_variable enqueue_cv_;           ///< stop 等待在途入队
  std::size_t queued_tasks_{0};                  ///< 已入队但尚未被 worker 领取的任务数
  std::size_t pending_tasks_{0};                 ///< 已接纳但尚未完成的任务数
  std::size_t enqueuers_{0};                     ///< 正在写入无锁队列的入队操作数
  bool accepting_tasks_{true};                   ///< 是否接受新任务
  bool stopping_{false};                         ///< worker 可在队列排空后退出
};

template <typename F, typename... Args>
  requires std::invocable<F, Args...>
void LockFreeThreadPool::enqueue_impl(F&& f, Args&&... args) {
  auto bound = [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::move(args)...); };
  MoveOnlyFunction task(std::move(bound));

  {
    std::lock_guard lock(state_mutex_);
    if (!accepting_tasks_) {
      return;
    }
    ++enqueuers_;
  }

  try {
    finish_enqueue(tasks_.push(std::move(task)));
  } catch (...) {
    finish_enqueue(false);
    throw;
  }
}

} // namespace hps
