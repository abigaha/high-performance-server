#pragma once

#include <cstddef>
#include <memory>

#include "tiered_memory_pool.h"

namespace hps {

// 内存池 deleter：static_cast 后 delete，静态分发（无 virtual）
inline void TieredMemoryPoolDeleter(MemoryPoolBase<TieredMemoryPool>* p) {
  delete static_cast<TieredMemoryPool*>(p);
}

using MemoryPoolPtr = std::unique_ptr<MemoryPoolBase<TieredMemoryPool>, decltype(&TieredMemoryPoolDeleter)>;

// 返回 CRTP 基类智能指针 + 自定义 deleter（静态析构，无 virtual 开销）
inline MemoryPoolPtr CreateMemoryPool() {
  return MemoryPoolPtr(new TieredMemoryPool(), &TieredMemoryPoolDeleter);
}

}  // namespace hps
