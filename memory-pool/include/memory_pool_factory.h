#pragma once

#include <memory>

#include "memory_pool_base.h"
#include "tiered_memory_pool.h"

namespace hps {

// 返回 CRTP 基类智能指针，消费者依赖 MemoryPoolBase<TieredMemoryPool> 接口（静态多态，内联）
auto CreateMemoryPool() -> std::unique_ptr<MemoryPoolBase<TieredMemoryPool>>;

}  // namespace hps
