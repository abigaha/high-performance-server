#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

namespace hps {

template <typename T, std::size_t Capacity = 1023, typename Allocator = std::allocator<T>>
class LockFreeQueue {
  static_assert(Capacity > 0, "Capacity必须大于0");
  static_assert((Capacity & (Capacity + 1)) == 0, "Capacity必须是2的幂次方减1");

public:
  LockFreeQueue();
  LockFreeQueue(const LockFreeQueue&) = delete;
  LockFreeQueue& operator=(const LockFreeQueue&) = delete;
  LockFreeQueue& operator=(const LockFreeQueue&) volatile = delete;
  ~LockFreeQueue();

  inline bool empty();
  inline bool full();
  inline void stop();

  inline bool is_stopped() const { return stop_token_.stop_requested(); }

  template <typename... Args>
  auto emplace(Args&&... args) -> bool;
  bool push(const T& item);
  bool push(T&& item);
  bool pop(T& item);
  bool try_pop(T& item);
  T pop();

private:
  inline static std::uint32_t get_version(std::uint64_t index) { return static_cast<std::uint32_t>(index >> 32U); }

  inline static std::uint32_t get_index(std::uint64_t index) { return static_cast<std::uint32_t>(index & 0xFFFFFFFF); }

  inline static std::uint64_t make_index(std::uint32_t version, std::uint32_t index) {
    return (static_cast<std::uint64_t>(version) << 32U) | index;
  }

  inline static std::uint64_t increment_index(std::uint64_t index) {
    std::uint32_t version = get_version(index);
    std::uint32_t idx = get_index(index);
    idx = (idx + 1) & Capacity;
    if (idx == 0) {
      version++;
    }
    return make_index(version, idx);
  }

private:
  enum class State : uint8_t { EMPTY, PUSHED, POPPING, ABORTING };

private:
  [[no_unique_address]] Allocator allocator_;
  const std::size_t capacity_;
  T* buffer_;
  std::vector<std::atomic<State>, typename std::allocator_traits<Allocator>::template rebind_alloc<std::atomic<State>>>
    state_;
  std::atomic<std::uint64_t> head_;
  std::atomic<std::uint64_t> tail_;
  std::stop_source stop_source_;
  std::stop_token stop_token_;
};

template <typename T, std::size_t Capacity, typename Allocator>
LockFreeQueue<T, Capacity, Allocator>::LockFreeQueue() :
    capacity_{Capacity + 1},
    buffer_{std::allocator_traits<decltype(allocator_)>::allocate(allocator_, capacity_)},
    head_{0},
    tail_{0},
    state_{capacity_},
    // NOLINTNEXTLINE(readability-redundant-member-init)
    stop_source_{},
    stop_token_{stop_source_.get_token()} {
  for (auto& s : state_) {
    s.store(State::EMPTY, std::memory_order_relaxed);
  }
}

template <typename T, std::size_t Capacity, typename Allocator>
LockFreeQueue<T, Capacity, Allocator>::~LockFreeQueue() {
  while (!empty()) {
    std::uint64_t current_head = head_.load(std::memory_order_relaxed);
    std::uint32_t head_index = get_index(current_head);
    std::allocator_traits<decltype(allocator_)>::destroy(allocator_,
                                                         std::next(buffer_, static_cast<std::ptrdiff_t>(head_index)));
    state_[head_index].store(State::EMPTY, std::memory_order_relaxed);
    head_.store(increment_index(current_head), std::memory_order_relaxed);
  }
  std::allocator_traits<decltype(allocator_)>::deallocate(allocator_, buffer_, capacity_);
}

template <typename T, std::size_t Capacity, typename Allocator>
inline bool LockFreeQueue<T, Capacity, Allocator>::empty() {
  return get_index(head_.load(std::memory_order_acquire)) == get_index(tail_.load(std::memory_order_acquire));
}

template <typename T, std::size_t Capacity, typename Allocator>
inline bool LockFreeQueue<T, Capacity, Allocator>::full() {
  return (get_index(tail_.load(std::memory_order_acquire)) + 1) % capacity_ ==
         get_index(head_.load(std::memory_order_acquire));
}

template <typename T, std::size_t Capacity, typename Allocator>
inline void LockFreeQueue<T, Capacity, Allocator>::stop() {
  stop_source_.request_stop();
}

template <typename T, std::size_t Capacity, typename Allocator>
template <typename... Args>
bool LockFreeQueue<T, Capacity, Allocator>::emplace(Args&&... args) {
  while (true) {
    if (stop_token_.stop_requested()) {
      return false;
    }
    while (full()) {
      if (stop_token_.stop_requested()) {
        return false;
      }
      std::this_thread::yield();
    }
    std::uint64_t current_tail = tail_.load(std::memory_order_acquire);
    std::uint64_t next_tail = increment_index(current_tail);
    if (tail_.compare_exchange_weak(current_tail, next_tail, std::memory_order_release, std::memory_order_relaxed)) {
      std::uint32_t tail_index = get_index(current_tail);
      while (state_[tail_index].load(std::memory_order_acquire) != State::EMPTY) {
        std::this_thread::yield();
      }
      try {
        std::allocator_traits<decltype(allocator_)>::construct(
          allocator_, std::next(buffer_, static_cast<std::ptrdiff_t>(tail_index)), std::forward<Args>(args)...);
      } catch (...) {
        state_[tail_index].store(State::ABORTING, std::memory_order_release);
        return false;
      }
      state_[tail_index].store(State::PUSHED, std::memory_order_release);
      return true;
    }
    std::this_thread::yield();
    // CAS 竞争失败，检查 stop 后重试
  }
}

template <typename T, std::size_t Capacity, typename Allocator>
bool LockFreeQueue<T, Capacity, Allocator>::push(const T& item) {
  return emplace(item);
}

template <typename T, std::size_t Capacity, typename Allocator>
// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
bool LockFreeQueue<T, Capacity, Allocator>::push(T&& item) {
  return emplace(std::forward<T>(item));
}

template <typename T, std::size_t Capacity, typename Allocator>
bool LockFreeQueue<T, Capacity, Allocator>::pop(T& item) {
  while (true) {
    if (stop_token_.stop_requested()) {
      return false;
    }
    while (empty()) {
      if (stop_token_.stop_requested()) {
        return false;
      }
      std::this_thread::yield();
    }
    std::uint64_t current_head = head_.load(std::memory_order_acquire);
    if (get_index(current_head) == get_index(tail_.load(std::memory_order_acquire))) {
      continue;
    }
    std::uint64_t next_head = increment_index(current_head);
    if (head_.compare_exchange_weak(current_head, next_head, std::memory_order_release, std::memory_order_relaxed)) {
      std::uint32_t head_index = get_index(current_head);
      while (state_[head_index].load(std::memory_order_acquire) != State::PUSHED) {
        if (state_[head_index].load(std::memory_order_acquire) == State::ABORTING) {
          state_[head_index].store(State::EMPTY, std::memory_order_release);
          return false;
        }
        if (stop_token_.stop_requested()) {
          state_[head_index].store(State::EMPTY, std::memory_order_release);
          return false;
        }
        std::this_thread::yield();
      }
      static_assert(std::is_nothrow_move_assignable_v<T>);
      item = std::move(*std::next(buffer_, static_cast<std::ptrdiff_t>(head_index)));
      state_[head_index].store(State::POPPING, std::memory_order_release);
      std::allocator_traits<decltype(allocator_)>::destroy(allocator_,
                                                           std::next(buffer_, static_cast<std::ptrdiff_t>(head_index)));
      state_[head_index].store(State::EMPTY, std::memory_order_release);
      return true;
    }
    std::this_thread::yield();
    // CAS 竞争失败，检查 stop 后重试
  }
}

template <typename T, std::size_t Capacity, typename Allocator>
bool LockFreeQueue<T, Capacity, Allocator>::try_pop(T& item) {
  while (true) {
    std::uint64_t current_head = head_.load(std::memory_order_acquire);
    if (get_index(current_head) == get_index(tail_.load(std::memory_order_acquire))) {
      return false;
    }
    const std::uint64_t next_head = increment_index(current_head);
    if (!head_.compare_exchange_weak(current_head, next_head, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      continue;
    }
    std::uint32_t head_index = get_index(current_head);
    while (state_[head_index].load(std::memory_order_acquire) != State::PUSHED) {
      if (state_[head_index].load(std::memory_order_acquire) == State::ABORTING) {
        state_[head_index].store(State::EMPTY, std::memory_order_release);
        return false;
      }
      if (stop_token_.stop_requested()) {
        state_[head_index].store(State::EMPTY, std::memory_order_release);
        return false;
      }
      std::this_thread::yield();
    }
    static_assert(std::is_nothrow_move_assignable_v<T>);
    item = std::move(*std::next(buffer_, static_cast<std::ptrdiff_t>(head_index)));
    state_[head_index].store(State::POPPING, std::memory_order_release);
    std::allocator_traits<decltype(allocator_)>::destroy(allocator_,
                                                         std::next(buffer_, static_cast<std::ptrdiff_t>(head_index)));
    state_[head_index].store(State::EMPTY, std::memory_order_release);
    return true;
  }
}

template <typename T, std::size_t Capacity, typename Allocator>
T LockFreeQueue<T, Capacity, Allocator>::pop() {
  static_assert(std::is_default_constructible_v<T>, "T必须是可默认构造的");
  T item;
  while (!pop(item)) {
    if (stop_token_.stop_requested()) {
      break;
    }
    std::this_thread::yield();
  }
  return item;
}

} // namespace hps
