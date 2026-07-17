#include "thread_pool.h"

#include <thread>

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
  for (auto& w : workers_) {
    if (w.joinable()) {
      w.join();
    }
  }
}

void LockFreeThreadPool::worker(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    MoveOnlyFunction task;
    if (!tasks_.pop(task)) {
      std::this_thread::yield();
      continue;
    }
    task();
    pending_.fetch_sub(1, std::memory_order_release);
  }
}

void LockFreeThreadPool::wait_impl() {
  while (pending_.load(std::memory_order_acquire) > 0) {
    if (tasks_.is_stopped())
      return;
    std::this_thread::yield();
  }
}

} // namespace hps