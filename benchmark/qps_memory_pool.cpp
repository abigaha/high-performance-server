#include "memory_pool_factory.h"
#include "qps_runner.hpp"

#include <cstddef>
#include <memory>
#include <vector>

int main() {
  auto levels = hps::bench::default_qps_levels();

  // Small alloc (32 bytes)
  {
    auto pool = hps::CreateMemoryPool();
    hps::bench::run_qps_steps("MemoryPool Alloc/Dealloc 32B", levels, [&pool](int) {
      void* p = pool->allocate(32);
      pool->deallocate(p, 32);
    });
  }

  // Medium alloc (512 bytes)
  {
    auto pool = hps::CreateMemoryPool();
    hps::bench::run_qps_steps("MemoryPool Alloc/Dealloc 512B", levels, [&pool](int) {
      void* p = pool->allocate(512);
      pool->deallocate(p, 512);
    });
  }

  // Large alloc (4096 bytes)
  {
    auto pool = hps::CreateMemoryPool();
    hps::bench::run_qps_steps("MemoryPool Alloc/Dealloc 4KB", levels, [&pool](int) {
      void* p = pool->allocate(4096);
      pool->deallocate(p, 4096);
    });
  }

  return 0;
}
