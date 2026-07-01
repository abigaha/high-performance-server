#pragma once

#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace hps {

/**
 * 轻量 move-only 可调用对象封装（SBO 小对象优化）
 *
 * 替代 std::function<void()>，避免堆分配：
 * - sizeof(F) <= kInlineSize 且对齐满足时，lambda 直接栈存储（SBO）
 * - 超过则堆分配
 * - move-only，不可 copy
 *
 * 通过 vtable（函数指针组）实现类型擦除，调用点仅一次间接跳转，
 * 无 std::function 的异常传播/RTTI 开销。
 */
class MoveOnlyFunction {
public:
  MoveOnlyFunction() noexcept = default;

  ~MoveOnlyFunction() { reset(); }

  MoveOnlyFunction(const MoveOnlyFunction&) = delete;
  MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

  // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
  MoveOnlyFunction(MoveOnlyFunction&& other) noexcept { move_from(std::move(other)); }

  MoveOnlyFunction& operator=(MoveOnlyFunction&& other) noexcept {
    if (this != &other) {
      reset();
      move_from(std::move(other));
    }
    return *this;
  }

  /**
   * 从可调用对象 F 构造（SBO 判断）
   * @tparam F 可调用类型，需满足 std::invocable<F>
   */
  template <typename F>
    requires std::invocable<F>
  // NOLINTNEXTLINE(bugprone-forwarding-reference-overload,hicpp-explicit-conversions,noExplicitConstructor)
  explicit MoveOnlyFunction(F&& f) {
    using DecayedF = std::decay_t<F>;
    static_assert(std::is_nothrow_move_constructible_v<DecayedF>, "F must be nothrow move constructible");

    if constexpr (sizeof(DecayedF) <= kInlineSize && alignof(DecayedF) <= kInlineAlign) {
      // SBO：栈存储
      heap_ = false;
      // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
      ::new (storage_) DecayedF(std::forward<F>(f));
      vtable_ = &sbo_vtable<DecayedF>;
    } else {
      // 堆存储
      heap_ = true;
      // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
      *reinterpret_cast<DecayedF**>(storage_) = new DecayedF(std::forward<F>(f));
      vtable_ = &heap_vtable<DecayedF>;
    }
  }

  /** 调用封装的可调用对象 */
  void operator()() {
    if (vtable_ != nullptr) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
      vtable_->invoke(storage_);
    }
  }

  /** 是否持有效可调用对象 */
  explicit operator bool() const noexcept { return vtable_ != nullptr; }

  /** 释放持有的可调用对象 */
  void reset() noexcept {
    if (vtable_ != nullptr) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
      vtable_->destroy(storage_);
      vtable_ = nullptr;
      heap_ = false;
    }
  }

private:
  static constexpr std::size_t kInlineSize = 32; ///< SBO 阈值（字节）
  static constexpr std::size_t kInlineAlign = alignof(std::max_align_t);

  struct VTable {
    void (*invoke)(void* storage);
    void (*move)(void* from, void* to);
    void (*destroy)(void* storage);
  };

  // SBO 模式的 vtable
  template <typename F>
  static const VTable sbo_vtable;

  // 堆存储模式的 vtable
  template <typename F>
  static const VTable heap_vtable;

  const VTable* vtable_{nullptr};
  bool heap_{false};
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
  alignas(kInlineAlign) std::byte storage_[kInlineSize]; // SBO 存储（C 数组因 placement new 需求，不用 std::array）

  // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
  void move_from(MoveOnlyFunction&& other) noexcept {
    if (other.vtable_ == nullptr) {
      return;
    }
    vtable_ = other.vtable_;
    heap_ = other.heap_;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
    vtable_->move(other.storage_, storage_);
    other.vtable_ = nullptr;
    other.heap_ = false;
  }
};

// SBO vtable 实现
template <typename F>
const MoveOnlyFunction::VTable MoveOnlyFunction::sbo_vtable = {
  // invoke
  [](void* storage) {
    auto* f = static_cast<F*>(storage);
    (*f)();
  },
  // move
  [](void* from, void* to) {
    auto* src = static_cast<F*>(from);
    ::new (to) F(std::move(*src));
    src->~F();
  },
  // destroy
  [](void* storage) { static_cast<F*>(storage)->~F(); }};

// 堆存储 vtable 实现
template <typename F>
const MoveOnlyFunction::VTable MoveOnlyFunction::heap_vtable = {
  // invoke
  [](void* storage) {
    auto* f = *reinterpret_cast<F**>(storage);
    (*f)();
  },
  // move
  [](void* from, void* to) {
    auto*& src = *reinterpret_cast<F**>(from);
    *reinterpret_cast<F**>(to) = src;
    src = nullptr;
  },
  // destroy
  [](void* storage) {
    auto*& f = *reinterpret_cast<F**>(storage);
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    delete f;
    f = nullptr;
  }};

} // namespace hps
