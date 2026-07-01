#include "memory_pool_factory.h"

#include "tiered_memory_pool.h"

namespace hps {

auto CreateMemoryPool() -> std::unique_ptr<MemoryPoolBase<TieredMemoryPool>> {
  return std::make_unique<TieredMemoryPool>();
}

}  // namespace hps
