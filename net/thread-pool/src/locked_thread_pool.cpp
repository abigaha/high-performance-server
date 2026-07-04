#include "locked_thread_pool.h"

#include "logger.h"

namespace hps {

LockedThreadPool::LockedThreadPool(std::size_t num_threads) {
  workers_.reserve(num_threads);
  for (std::size_t i = 0; i < num_threads; ++i) {
    workers_.emplace_back([this](std::stop_token st) { worker(std::move(st)); });
  }
}

LockedThreadPool::~LockedThreadPool() {
  if (!stop_) {
    stop_impl();
  }
}

void LockedThreadPool::worker(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    MoveOnlyFunction task;
    {
      std::unique_lock lock(queue_mutex_);
      cv_.wait(lock, [this, &stop_token] { return stop_ || !tasks_.empty() || stop_token.stop_requested(); });
      if (stop_ || stop_token.stop_requested()) {
        break;
      }
      if (tasks_.empty()) {
        continue;
      }
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    try {
      task();
    } catch (const std::exception& e) {
      Logger::_error(std::string("Unhandled exception in worker: ") + e.what());
    } catch (...) {
      Logger::_error("Unhandled unknown exception in worker");
    }
    pending_.fetch_sub(1, std::memory_order_release);
    cv_.notify_all();
  }
}

void LockedThreadPool::wait_impl() {
  std::unique_lock lock(queue_mutex_);
  cv_.wait(lock, [this] { return pending_.load(std::memory_order_acquire) == 0; });
}

bool LockedThreadPool::wait_for(std::chrono::milliseconds timeout) {
  std::unique_lock lock(queue_mutex_);
  return cv_.wait_for(lock, timeout, [this] { return pending_.load(std::memory_order_acquire) == 0; });
}

void LockedThreadPool::stop_impl() {
  {
    std::lock_guard lock(queue_mutex_);
    stop_ = true;
    std::queue<MoveOnlyFunction> empty;
    std::swap(tasks_, empty);
    pending_.store(0, std::memory_order_release);
  }
  cv_.notify_all();
  for (auto& w : workers_) {
    w.request_stop();
  }
  for (auto& w : workers_) {
    if (w.joinable()) {
      w.join();
    }
  }
}

} // namespace hps
