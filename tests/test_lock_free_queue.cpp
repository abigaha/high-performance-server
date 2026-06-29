#include "lock_free_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace hps;

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// T1: 单线程基本 push/pop
TEST(LockFreeQueueTest, BasicPushPop) {
  LockFreeQueue<int, 127> queue;

  EXPECT_TRUE(queue.push(1));
  EXPECT_TRUE(queue.push(2));
  EXPECT_TRUE(queue.push(3));

  int val{};
  EXPECT_TRUE(queue.pop(val));
  EXPECT_EQ(val, 1);
  EXPECT_TRUE(queue.pop(val));
  EXPECT_EQ(val, 2);
  EXPECT_TRUE(queue.pop(val));
  EXPECT_EQ(val, 3);
}

// T2: 多 producer + 多 consumer 并发——验证 empty() ordering 正确
TEST(LockFreeQueueTest, ConcurrentPushPop) {
  LockFreeQueue<int, 1023> queue;
  std::atomic<int> produced{0};
  std::atomic<int> consumed{0};
  constexpr int kNumItems = 5000;

  std::thread producer([&]() {
    for (int i = 0; i < kNumItems; ++i) {
      while (!queue.push(i)) {
        ;
      }
      produced.fetch_add(1, std::memory_order_relaxed);
    }
  });

  std::thread consumer([&]() {
    int last = -1;
    for (int i = 0; i < kNumItems; ++i) {
      int val = 0;
      while (!queue.pop(val)) {
        ;
      }
      EXPECT_GE(val, last);
      last = val;
      consumed.fetch_add(1, std::memory_order_relaxed);
    }
  });

  producer.join();
  consumer.join();

  EXPECT_EQ(produced.load(), kNumItems);
  EXPECT_EQ(consumed.load(), kNumItems);
}

// T3: stop 后 pop 应返回 false
TEST(LockFreeQueueTest, StopQueue) {
  LockFreeQueue<int, 127> queue;

  queue.stop();

  int val = 42;
  EXPECT_FALSE(queue.pop(val));
}
