#include "thread_pool.h"

#include <exception>
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
  std::lock_guard stop_lock(stop_mutex_);

  {
    std::unique_lock state_lock(state_mutex_);
    if (stopping_) {
      return;
    }

    accepting_tasks_ = false;
    enqueue_cv_.wait(state_lock, [this] { return enqueuers_ == 0; });
    stopping_ = true;
  }

  work_cv_.notify_all();
  for (auto& w : workers_) {
    if (w.joinable()) {
      w.join();
    }
  }

  tasks_.stop();
}

void LockFreeThreadPool::worker([[maybe_unused]] std::stop_token stop_token) {
  while (true) {
    MoveOnlyFunction task;

    {
      std::unique_lock state_lock(state_mutex_);
      work_cv_.wait(state_lock, [this] { return stopping_ || queued_tasks_ > 0; });
      if (queued_tasks_ == 0) {
        return;
      }
      --queued_tasks_;
    }

    if (!tasks_.try_pop(task)) {
      std::terminate();
    }

    task();

    bool notify_idle = false;
    {
      std::lock_guard state_lock(state_mutex_);
      --pending_tasks_;
      notify_idle = pending_tasks_ == 0 && enqueuers_ == 0;
    }
    if (notify_idle) {
      idle_cv_.notify_all();
    }
  }
}

void LockFreeThreadPool::wait_impl() {
  std::unique_lock state_lock(state_mutex_);
  idle_cv_.wait(state_lock, [this] { return pending_tasks_ == 0 && enqueuers_ == 0; });
}

void LockFreeThreadPool::finish_enqueue(bool task_enqueued) {
  bool notify_idle = false;
  {
    std::lock_guard state_lock(state_mutex_);
    --enqueuers_;
    if (task_enqueued) {
      ++queued_tasks_;
      ++pending_tasks_;
    }
    notify_idle = pending_tasks_ == 0 && enqueuers_ == 0;
  }

  enqueue_cv_.notify_all();
  if (task_enqueued) {
    work_cv_.notify_one();
  }
  if (notify_idle) {
    idle_cv_.notify_all();
  }
}

} // namespace hps
