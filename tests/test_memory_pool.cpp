#include "i_memory_pool.h"
#include "memory_pool_factory.h"
#include "size_class.h"
#include "tiered_memory_pool.h"

#include <gtest/gtest.h>

#include <set>
#include <thread>
#include <vector>

using namespace hps;

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// TC1: 工厂创建 + 接口多态性验证
TEST(MemoryPoolTest, FactoryCreate) {
  auto pool = CreateMemoryPool();
  ASSERT_NE(pool, nullptr);
  auto* ptr = pool->allocate(64);
  ASSERT_NE(ptr, nullptr);
  pool->deallocate(ptr, 64);
}

// TC2: 基本分配释放，不崩溃
TEST(MemoryPoolTest, BasicAllocDealloc) {
  auto pool = CreateMemoryPool();
  void* ptr = pool->allocate(64);
  ASSERT_NE(ptr, nullptr);
  EXPECT_NO_THROW(pool->deallocate(ptr, 64));
}

// TC3: 多种 size class 交叉分配，地址不重叠
TEST(MemoryPoolTest, MultiSizeClass) {
  auto pool = CreateMemoryPool();

  std::vector<void*> ptrs8, ptrs64, ptrs256;
  for (int i = 0; i < 20; ++i) ptrs8.push_back(pool->allocate(8));
  for (int i = 0; i < 20; ++i) ptrs64.push_back(pool->allocate(64));
  for (int i = 0; i < 20; ++i) ptrs256.push_back(pool->allocate(256));

  std::set<void*> unique(ptrs8.begin(), ptrs8.end());
  unique.insert(ptrs64.begin(), ptrs64.end());
  unique.insert(ptrs256.begin(), ptrs256.end());
  EXPECT_EQ(unique.size(), ptrs8.size() + ptrs64.size() + ptrs256.size());

  for (auto* p : ptrs8) pool->deallocate(p, 8);
  for (auto* p : ptrs64) pool->deallocate(p, 64);
  for (auto* p : ptrs256) pool->deallocate(p, 256);
}

// TC4: 耗尽扩容
TEST(MemoryPoolTest, ExpandOnDemand) {
  auto pool = CreateMemoryPool();

  std::vector<void*> ptrs;
  for (int i = 0; i < 100; ++i) {
    auto* p = pool->allocate(64);
    ASSERT_NE(p, nullptr);
    ptrs.push_back(p);
  }
  for (auto* p : ptrs) pool->deallocate(p, 64);
}

// TC5: 释放后复用
TEST(MemoryPoolTest, ReuseAfterFree) {
  auto pool = CreateMemoryPool();
  auto* p1 = pool->allocate(64);
  pool->deallocate(p1, 64);
  auto* p2 = pool->allocate(64);
  EXPECT_EQ(p1, p2);
  pool->deallocate(p2, 64);
}

// TC6: 合并验证（通过多次分配和释放一个 size class 触发合并）
TEST(MemoryPoolTest, MergeOnOverflow) {
  auto pool = CreateMemoryPool();

  // 分配并释放大量 8B 块，使 L1 的 8B 桶溢出并触发合并
  std::vector<void*> ptrs;
  for (int i = 0; i < 50; ++i) ptrs.push_back(pool->allocate(8));
  for (auto* p : ptrs) pool->deallocate(p, 8);

  // 再次分配应仍然正常
  for (int i = 0; i < 50; ++i) {
    auto* p = pool->allocate(8);
    ASSERT_NE(p, nullptr);
    pool->deallocate(p, 8);
  }
}

// TC7: 多线程并发
TEST(MemoryPoolTest, MultiThreadConcurrent) {
  auto pool = CreateMemoryPool();

  constexpr int kThreads = 4;
  constexpr int kIters = 500;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&pool]() {
      for (int i = 0; i < kIters; ++i) {
        auto* p = pool->allocate(64);
        ASSERT_NE(p, nullptr);
        *static_cast<char*>(p) = 'x';
        pool->deallocate(p, 64);
      }
    });
  }
  for (auto& th : threads) th.join();
}

// TC8: 析构安全——其他线程 TL cache 持有本池块
TEST(MemoryPoolTest, DestroyWithCrossThreadCache) {
  auto pool = CreateMemoryPool();

  std::thread t([&pool]() {
    for (int i = 0; i < 100; ++i) {
      void* p = pool->allocate(64);
      pool->deallocate(p, 64);
    }
  });
  t.join();

  // 析构时不访问其他线程的 TL cache，不 crash
  pool.reset();
  SUCCEED();
}

// TC9: SizeClass 工具函数测试
TEST(MemoryPoolTest, SizeClassUtils) {
  EXPECT_EQ(round_up_size(1), 8);
  EXPECT_EQ(round_up_size(8), 8);
  EXPECT_EQ(round_up_size(9), 16);
  EXPECT_EQ(round_up_size(17), 32);
  EXPECT_EQ(round_up_size(4096), 4096);
  EXPECT_EQ(round_up_size(5000), 4096);

  EXPECT_EQ(size_class_index(8), 0);
  EXPECT_EQ(size_class_index(16), 1);
  EXPECT_EQ(size_class_index(32), 2);
  EXPECT_EQ(size_class_index(4096), 9);

  EXPECT_EQ(size_class_size(0), 8);
  EXPECT_EQ(size_class_size(1), 16);
  EXPECT_EQ(size_class_size(9), 4096);
}
