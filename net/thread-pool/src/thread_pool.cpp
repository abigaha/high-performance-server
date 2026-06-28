#include "thread_pool.h"

namespace hps {

ThreadPool::ThreadPool(size_t numThreads) {
  for (size_t i = 0; i < numThreads; ++i) {
    workers_.emplace_back([this](std::stop_token st) { worker(st); });
  }
}

ThreadPool::~ThreadPool() {
  // 顺序：先停止任务队列（pop 返回 false），再请求线程停止，最后 join
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

void ThreadPool::worker(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    std::function<void()> task;
    // pop 返回 false 说明队列已 stop 或为空
    if (!tasks_.pop(task)) {
      continue;
    }
    task();
  }
}

void ThreadPool::wait_for_all_tasks() {
  while (true) {
    if (tasks_.empty()) {
      break;
    }
    std::this_thread::yield();
  }
}

} // namespace hps
