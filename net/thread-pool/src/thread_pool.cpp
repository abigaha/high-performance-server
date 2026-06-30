#include "thread_pool.h"

#include <mutex>

namespace hps {

ThreadPool::ThreadPool(size_t numThreads) {
  for (size_t i = 0; i < numThreads; ++i) {
    workers_.emplace_back([this](std::stop_token st) { worker(st); });
  }
}

ThreadPool::~ThreadPool() {
  tasks_.stop();
  for (auto& w : workers_) {
    w.request_stop();
  }
  cv_.notify_all();
  for (auto& w : workers_) {
    if (w.joinable()) {
      w.join();
    }
  }
}

void ThreadPool::worker(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    std::function<void()> task;
    {
      std::unique_lock lock(cv_mutex_);
      cv_.wait(lock, [this, &stop_token] {
        return stop_token.stop_requested() || pending_.load(std::memory_order_acquire) > 0;
      });
    }
    if (stop_token.stop_requested()) {
      break;
    }
    // N3-M：try_pop 失败说明被竞争抢走，循环重试而非立即回 wait（避免忙等）
    while (!stop_token.stop_requested()) {
      if (tasks_.try_pop(task)) {
        task();
        if (pending_.fetch_sub(1, std::memory_order_release) == 1) {
          cv_.notify_one();
        }
        break;
      }
      // 被抢，但 pending_>0 说明还有任务；若 pending_==0 说明被别人取完
      if (pending_.load(std::memory_order_acquire) == 0) {
        break;
      }
      std::this_thread::yield();
    }
  }
}

void ThreadPool::wait_for_all_tasks() {
  std::unique_lock lock(cv_mutex_);
  cv_.wait(lock, [this] { return pending_.load(std::memory_order_acquire) == 0; });
}

} // namespace hps
