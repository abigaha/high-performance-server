#include "memory_pool_factory.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

constexpr std::size_t kMediumPool = 512;
constexpr std::size_t kLargePool = 4096;

} // namespace

static void BM_MemoryPool_AllocateDeallocate(benchmark::State& state) {
  auto pool = hps::CreateMemoryPool();
  std::size_t size = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    void* p = pool->allocate(size);
    benchmark::DoNotOptimize(p);
    pool->deallocate(p, size);
  }
}

BENCHMARK(BM_MemoryPool_AllocateDeallocate)->Arg(32)->Arg(64)->Arg(128)->Arg(kMediumPool)->Arg(kLargePool);

static void BM_MemoryPool_BatchAllocate(benchmark::State& state) {
  auto pool = hps::CreateMemoryPool();
  std::size_t size = static_cast<std::size_t>(state.range(0));
  int count = state.range(1);
  std::vector<void*> ptrs(static_cast<std::size_t>(count));
  for (auto _ : state) {
    for (int i = 0; i < count; ++i) {
      ptrs[static_cast<std::size_t>(i)] = pool->allocate(size);
    }
    for (int i = 0; i < count; ++i) {
      pool->deallocate(ptrs[static_cast<std::size_t>(i)], size);
    }
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * count);
}

BENCHMARK(BM_MemoryPool_BatchAllocate)->Args({32, 100})->Args({kMediumPool, 100})->Args({kLargePool, 100});

static void BM_MemoryPool_MixedSizes(benchmark::State& state) {
  auto pool = hps::CreateMemoryPool();
  std::vector<std::size_t> sizes = {16, 32, 64, 128, kMediumPool, kLargePool};
  std::vector<void*> ptrs(sizes.size());
  for (auto _ : state) {
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      ptrs[i] = pool->allocate(sizes[i]);
    }
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      pool->deallocate(ptrs[i], sizes[i]);
    }
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * sizes.size());
}

BENCHMARK(BM_MemoryPool_MixedSizes);

static void BM_MemoryPool_CompareMalloc(benchmark::State& state) {
  std::size_t size = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    void* p = std::malloc(size);
    benchmark::DoNotOptimize(p);
    std::free(p);
  }
}

BENCHMARK(BM_MemoryPool_CompareMalloc)->Arg(32)->Arg(64)->Arg(128)->Arg(kMediumPool)->Arg(kLargePool);

BENCHMARK_MAIN();
