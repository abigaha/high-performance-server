#pragma once

#include <cstddef>

namespace hps {

/**
 * 内存池 CRTP 基类（静态多态）
 *
 * 高频调用路径（协程 new/delete）使用 CRTP 消除虚函数开销。
 * 派生类需实现 allocate_impl / deallocate_impl。
 *
 * @tparam Derived 派生类类型（如 TieredMemoryPool）
 */
template <typename Derived>
class MemoryPoolBase {
public:
  /** 分配 size 字节内存（转发到派生类 allocate_impl） */
  void* allocate(std::size_t size) noexcept {
    return static_cast<Derived*>(this)->allocate_impl(size);
  }

  /** 释放 ptr 指向的 size 字节内存（转发到派生类 deallocate_impl） */
  void deallocate(void* ptr, std::size_t size) noexcept {
    static_cast<Derived*>(this)->deallocate_impl(ptr, size);
  }

protected:
  MemoryPoolBase() = default;

public:
  // 析构 public virtual：unique_ptr<基类> 删除时正确调派生类析构。
  // allocate/deallocate 仍走 CRTP 内联（高频路径），析构虚开销仅在销毁时一次，可接受。
  virtual ~MemoryPoolBase() = default;

  MemoryPoolBase(const MemoryPoolBase&) = delete;
  MemoryPoolBase& operator=(const MemoryPoolBase&) = delete;
};

}  // namespace hps
