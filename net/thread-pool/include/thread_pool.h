#pragma once

#include "lock_free_queue.hpp"
#include "move_only_function.h"

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
 * 基于无锁队列 + 条件变量的线程池
 *
 * 内部以 LockFreeQueue<MoveOnlyFunction> 作为无锁任务队列，
 * 配合 std::condition_variable 实现工作线程的休眠唤醒，避免忙等自旋。
 * 任务通过 MoveOnlyFunction（SBO 优化）存储，避免 std::function 堆分配。
 */
class ThreadPool {
public:
  /**
   * 构造函数
   * @param numThreads 工作线程数（0 表示不创建工作线程）
   */
  explicit ThreadPool(size_t numThreads);

  ~ThreadPool();

  /**
   * 投递任务到线程池
   * @tparam F 可调用对象类型（lambda 等），需满足 std::invocable
   * @tparam Args 绑定参数类型
   * @param f 可调用对象
   * @param args 绑定参数
   */
  template <typename F, typename... Args>
    requires std::invocable<F, Args...>
  void enqueue(F&& f, Args&&... args);

  /** 等待所有已投递任务完成 */
  void wait_for_all_tasks();

private:
  /** 工作线程主函数 */
  void worker(std::stop_token stop_token);

  std::vector<std::jthread> workers_;     ///< 工作线程集合（jthread 自动 join）
  LockFreeQueue<MoveOnlyFunction> tasks_; ///< 无锁任务队列（MoveOnlyFunction SBO 优化）
  std::mutex cv_mutex_;                   ///< 条件变量互斥量
  std::condition_variable cv_;            ///< 任务到达通知
  std::atomic<int> pending_{0};           ///< 待处理任务数
};

template <typename F, typename... Args>
  requires std::invocable<F, Args...>
void ThreadPool::enqueue(F&& f, Args&&... args) {
  // 绑定参数到 lambda（C++20 pack expansion in lambda init-capture）
  auto bound = [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::move(args)...); };
  // N4-M：检查 push 返回值，失败时不增 pending_ 避免任务丢失导致 wait_for_all_tasks 死等
  bool ok = tasks_.push(MoveOnlyFunction(std::move(bound)));
  if (!ok) {
    return; // 队列已 stop，任务丢弃
  }
  pending_.fetch_add(1, std::memory_order_release);
  cv_.notify_one();
}

} // namespace hps
