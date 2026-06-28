#pragma once

#include "lock_free_queue.hpp"

#include <functional>
#include <stop_token>
#include <thread>
#include <vector>

namespace hps {

/**
 * 基于无锁队列的线程池
 *
 * 使用 std::jthread 实现自动 join 语义，内部以 LockFreeQueue 作为任务队列，
 * 支持任务投递（enqueue）和优雅停止。
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

  /** 等待所有已投递任务完成（已弃用：析构时直接 stop + join） */
  void wait_for_all_tasks();

private:
  /** 工作线程主函数 */
  void worker(std::stop_token stop_token);

  std::vector<std::jthread> workers_;          ///< 工作线程集合（jthread 自动 join）
  LockFreeQueue<std::function<void()>> tasks_; ///< 无锁任务队列
};

template <class F, class... Args>
void ThreadPool::enqueue(F&& f, Args&&... args) {
  tasks_.push([f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::move(args)...); });
}

} // namespace hps
