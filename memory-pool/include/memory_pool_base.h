#pragma once

#include <cstddef>

namespace hps {

/**
 * 内存池 CRTP 基类（静态多态）
 *
 * 高频调用路径（协程 new/delete）使用 CRTP 消除虚函数开销。
 * 派生类需实现 allocate_impl / deallocate_impl。
 *
 * 析构为 protected non-virtual，通过工厂返回的自定义 deleter 静态分发到派生类。
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

  MemoryPoolBase(const MemoryPoolBase&) = delete;
  MemoryPoolBase& operator=(const MemoryPoolBase&) = delete;

protected:
  MemoryPoolBase() = default;
  ~MemoryPoolBase() = default;  // non-virtual：析构通过自定义 deleter 静态分发到派生类
};

}  // namespace hps
