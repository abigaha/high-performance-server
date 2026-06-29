#pragma once

#include "lock_free_queue.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace hps {

/**
 * 基于无锁队列 + 条件变量的线程池
 *
 * 内部以 LockFreeQueue 作为无锁任务队列，配合 std::condition_variable
 * 实现工作线程的休眠唤醒，避免忙等自旋。
 */
class ThreadPool {
public:
  /**
   * 构造函数
   * @param numThreads 工作线程数（0 表示不创建工作线程）
   */
  explicit ThreadPool(size_t numThreads);

  ~ThreadPool();

  /** 投递任务到线程池 */
  template <class F, class... Args>
  void enqueue(F&& f, Args&&... args);

  /** 等待所有已投递任务完成 */
  void wait_for_all_tasks();

private:
  /** 工作线程主函数 */
  void worker(std::stop_token stop_token);

  std::vector<std::jthread> workers_;          ///< 工作线程集合（jthread 自动 join）
  LockFreeQueue<std::function<void()>> tasks_; ///< 无锁任务队列
  std::mutex cv_mutex_;                        ///< 条件变量互斥量
  std::condition_variable cv_;                 ///< 任务到达通知
  std::atomic<int> pending_{0};                ///< 待处理任务数
};

template <class F, class... Args>
void ThreadPool::enqueue(F&& f, Args&&... args) {
  tasks_.push([f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::move(args)...); });
  pending_.fetch_add(1, std::memory_order_release);
  cv_.notify_one();
}


} // namespace hps
