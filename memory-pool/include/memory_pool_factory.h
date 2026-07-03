#pragma once

#include <cstddef>
#include <memory>

#include "tiered_memory_pool.h"

namespace hps {

// 内存池 deleter：static_cast 后 delete，静态分发（无 virtual）
inline void TieredMemoryPoolDeleter(MemoryPoolBase<TieredMemoryPool>* p) {
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cppcoreguidelines-pro-type-static-cast-downcast) CRTP 静态 deleter 模式：基类无 virtual 析构，需 static_cast 到派生类后 delete
  delete static_cast<TieredMemoryPool*>(p);
}

using MemoryPoolPtr = std::unique_ptr<MemoryPoolBase<TieredMemoryPool>, decltype(&TieredMemoryPoolDeleter)>;

// 返回 CRTP 基类智能指针 + 自定义 deleter（静态析构，无 virtual 开销）
inline MemoryPoolPtr CreateMemoryPool() {
  // NOLINTNEXTLINE(modernize-return-braced-init-list) braced init list 与自定义 deleter 构造不兼容
  return MemoryPoolPtr(new TieredMemoryPool(), &TieredMemoryPoolDeleter);
}

}  // namespace hps
