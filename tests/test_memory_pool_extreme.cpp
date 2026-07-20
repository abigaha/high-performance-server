#include "memory_pool_factory.h"
#include "tiered_memory_pool.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace hps;

TEST(MemoryPoolExtremeTest, AllocateZeroSize) {
  auto pool = CreateMemoryPool();
  auto* ptr = pool->allocate(0);
  // 0 字节分配可返回非空（最小对齐块）或 nullptr，两种都接受
  pool->deallocate(ptr, 0);
}

TEST(MemoryPoolExtremeTest, AllocateHugeSize) {
  auto pool = CreateMemoryPool();
  // 超过最大 size class，pool 回退到 kMaxBlockSize=4096
  auto* ptr = pool->allocate(8192);
  ASSERT_NE(ptr, nullptr);
  std::memset(ptr, 0xAB, 4096);
  pool->deallocate(ptr, 8192);
}

TEST(MemoryPoolExtremeTest, AllocateAndDeallocate) {
  auto pool = CreateMemoryPool();
  auto* ptr = pool->allocate(32);
  ASSERT_NE(ptr, nullptr);
  std::memcpy(ptr, "memory_pool_test", 16);
  pool->deallocate(ptr, 32);
}

TEST(MemoryPoolExtremeTest, MultipleSizes) {
  auto pool = CreateMemoryPool();
  std::vector<size_t> sizes = {8, 64, 512, 4096};
  std::vector<void*> ptrs;
  for (auto sz : sizes) {
    auto* p = pool->allocate(sz);
    ASSERT_NE(p, nullptr);
    std::memset(p, static_cast<int>(sz & 0xFF), sz);
    ptrs.push_back(p);
  }
  for (size_t i = 0; i < sizes.size(); ++i) {
    pool->deallocate(ptrs[i], sizes[i]);
  }
}

TEST(MemoryPoolExtremeTest, BatchAllocateDeallocate) {
  auto pool = CreateMemoryPool();
  constexpr int kCount = 100;
  std::vector<void*> ptrs;
  ptrs.reserve(kCount);
  for (int i = 0; i < kCount; ++i) {
    auto* p = pool->allocate(64);
    ASSERT_NE(p, nullptr);
    ptrs.push_back(p);
  }
  for (auto* p : ptrs) {
    pool->deallocate(p, 64);
  }
}

TEST(MemoryPoolExtremeTest, ReuseFreedMemory) {
  auto pool = CreateMemoryPool();
  auto* p1 = pool->allocate(64);
  ASSERT_NE(p1, nullptr);
  pool->deallocate(p1, 64);
  auto* p2 = pool->allocate(64);
  ASSERT_NE(p2, nullptr);
  // p1 和 p2 可能相同（LIFO 缓存复用），不强制断言
  pool->deallocate(p2, 64);
}

TEST(MemoryPoolExtremeTest, NullDeallocate) {
  auto pool = CreateMemoryPool();
  EXPECT_NO_THROW(pool->deallocate(nullptr, 0));
}

TEST(MemoryPoolExtremeTest, LargeBatchThenFree) {
  auto pool = CreateMemoryPool();
  constexpr int kCount = 1000;
  std::vector<void*> ptrs;
  ptrs.reserve(kCount);
  for (int i = 0; i < kCount; ++i) {
    auto* p = pool->allocate(128);
    ASSERT_NE(p, nullptr);
    ptrs.push_back(p);
  }
  for (auto* p : ptrs) {
    pool->deallocate(p, 128);
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
