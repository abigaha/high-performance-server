#include "thread_pool.h"

#include <mutex>

namespace hps {

LockFreeThreadPool::LockFreeThreadPool(size_t numThreads) {
  for (size_t i = 0; i < numThreads; ++i) {
    workers_.emplace_back([this](std::stop_token st) { worker(st); });
  }
}

LockFreeThreadPool::~LockFreeThreadPool() {
  stop_impl();
}

void LockFreeThreadPool::stop_impl() {
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

void LockFreeThreadPool::worker(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    MoveOnlyFunction task;
    {
      std::unique_lock lock(cv_mutex_);
      cv_.wait(lock, [this, &stop_token] {
        return stop_token.stop_requested() || pending_.load(std::memory_order_acquire) > 0;
      });
    }
    if (stop_token.stop_requested()) {
      break;
    }
    while (!stop_token.stop_requested()) {
      if (tasks_.try_pop(task)) {
        task();
        if (pending_.fetch_sub(1, std::memory_order_release) == 1) {
          cv_.notify_one();
        }
        break;
      }
      if (pending_.load(std::memory_order_acquire) == 0) {
        break;
      }
      std::this_thread::yield();
    }
  }
}

void LockFreeThreadPool::wait_impl() {
  std::unique_lock lock(cv_mutex_);
  cv_.wait(lock, [this] { return pending_.load(std::memory_order_acquire) == 0; });
}

} // namespace hps
