#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace hps {

class ChunkLifecycleCoordinator {
public:
  class CleanupGuard;

  class CleanupPermit {
  public:
    CleanupPermit(const CleanupPermit&) = delete;
    CleanupPermit& operator=(const CleanupPermit&) = delete;
    CleanupPermit(CleanupPermit&&) = delete;
    CleanupPermit& operator=(CleanupPermit&&) = delete;

    bool belongs_to(const ChunkLifecycleCoordinator& coordinator) const noexcept { return owner_ == &coordinator; }

  private:
    friend class ChunkLifecycleCoordinator::CleanupGuard;

    explicit CleanupPermit(const ChunkLifecycleCoordinator* owner) noexcept : owner_(owner) {}

    void invalidate() noexcept { owner_ = nullptr; }

    const ChunkLifecycleCoordinator* owner_{nullptr};
  };

  class UploadGuard {
  public:
    ~UploadGuard() { release(); }

    UploadGuard(const UploadGuard&) = delete;
    UploadGuard& operator=(const UploadGuard&) = delete;

    UploadGuard(UploadGuard&& other) noexcept :
        owner_(std::exchange(other.owner_, nullptr)),
        acquisition_thread_(std::exchange(other.acquisition_thread_, std::thread::id{})) {}

    UploadGuard& operator=(UploadGuard&& other) noexcept {
      if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        acquisition_thread_ = std::exchange(other.acquisition_thread_, std::thread::id{});
      }
      return *this;
    }

  private:
    friend class ChunkLifecycleCoordinator;

    UploadGuard(ChunkLifecycleCoordinator& owner, std::thread::id acquisition_thread) noexcept :
        owner_(&owner), acquisition_thread_(acquisition_thread) {}

    void release() noexcept {
      if (owner_ != nullptr) {
        owner_->release_upload(acquisition_thread_);
        owner_ = nullptr;
        acquisition_thread_ = {};
      }
    }

    ChunkLifecycleCoordinator* owner_;
    std::thread::id acquisition_thread_;
  };

  class CleanupGuard {
  public:
    ~CleanupGuard() { release(); }

    CleanupGuard(const CleanupGuard&) = delete;
    CleanupGuard& operator=(const CleanupGuard&) = delete;

    CleanupGuard(CleanupGuard&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), permit_(owner_) {
      other.permit_.invalidate();
    }

    CleanupGuard& operator=(CleanupGuard&&) = delete;

    const CleanupPermit& permit() const noexcept { return permit_; }

  private:
    friend class ChunkLifecycleCoordinator;

    explicit CleanupGuard(ChunkLifecycleCoordinator& owner) noexcept : owner_(&owner), permit_(&owner) {}

    void release() noexcept {
      if (owner_ != nullptr) {
        owner_->release_cleanup();
        owner_ = nullptr;
      }
    }

    ChunkLifecycleCoordinator* owner_;
    CleanupPermit permit_;
  };

  UploadGuard acquire_upload_guard() {
    std::unique_lock lock{mutex_};
    const auto acquisition_thread = std::this_thread::get_id();
    if (cleanup_active_ && cleanup_owner_thread_ == acquisition_thread) {
      throw std::logic_error("cleanup guard is already held by this thread");
    }
    condition_.wait(lock, [this] { return waiting_cleanup_ == 0 && !cleanup_active_; });
    ++upload_counts_by_thread_[acquisition_thread];
    ++active_uploads_;
    return UploadGuard{*this, acquisition_thread};
  }

  CleanupGuard acquire_cleanup_guard() {
    std::unique_lock lock{mutex_};
    const auto acquisition_thread = std::this_thread::get_id();
    if (cleanup_active_ && cleanup_owner_thread_ == acquisition_thread) {
      throw std::logic_error("cleanup guard is already held by this thread");
    }
    if (const auto held_uploads = upload_counts_by_thread_.find(acquisition_thread);
        held_uploads != upload_counts_by_thread_.end() && held_uploads->second != 0) {
      throw std::logic_error("upload guard is already held by this thread");
    }
    ++waiting_cleanup_;
    try {
      condition_.wait(lock, [this] { return active_uploads_ == 0 && !cleanup_active_; });
    } catch (...) {
      --waiting_cleanup_;
      condition_.notify_all();
      throw;
    }
    --waiting_cleanup_;
    cleanup_active_ = true;
    cleanup_owner_thread_ = acquisition_thread;
    return CleanupGuard{*this};
  }

  std::size_t active_uploads() const noexcept {
    std::lock_guard lock{mutex_};
    return active_uploads_;
  }

  std::size_t cleanup_waiters() const noexcept {
    std::lock_guard lock{mutex_};
    return waiting_cleanup_;
  }

private:
  void release_upload(std::thread::id acquisition_thread) noexcept {
    std::lock_guard lock{mutex_};
    const auto held_uploads = upload_counts_by_thread_.find(acquisition_thread);
    if (held_uploads != upload_counts_by_thread_.end() && --held_uploads->second == 0) {
      upload_counts_by_thread_.erase(held_uploads);
    }
    --active_uploads_;
    if (active_uploads_ == 0) {
      condition_.notify_all();
    }
  }

  void release_cleanup() noexcept {
    std::lock_guard lock{mutex_};
    cleanup_active_ = false;
    cleanup_owner_thread_ = {};
    condition_.notify_all();
  }

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t active_uploads_{0};
  std::unordered_map<std::thread::id, std::size_t> upload_counts_by_thread_;
  std::size_t waiting_cleanup_{0};
  bool cleanup_active_{false};
  std::thread::id cleanup_owner_thread_;
};

} // namespace hps
