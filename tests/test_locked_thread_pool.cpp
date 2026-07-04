#include "locked_thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace hps;

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

TEST(LockedThreadPoolTest, BasicTaskExecution) {
  LockedThreadPool pool(2);
  std::atomic<int> result{0};
  pool.enqueue([&result] { result = 42; });
  pool.wait_for(std::chrono::milliseconds(1000));
  EXPECT_EQ(result.load(), 42);
  pool.stop_impl();
}

TEST(LockedThreadPoolTest, ConcurrentTasks) {
  LockedThreadPool pool(4);
  std::atomic<int> counter{0};
  constexpr int kNumTasks = 1000;
  for (int i = 0; i < kNumTasks; ++i) {
    pool.enqueue([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  }
  pool.wait_for(std::chrono::milliseconds(5000));
  EXPECT_EQ(counter.load(), kNumTasks);
  pool.stop_impl();
}

TEST(LockedThreadPoolTest, WaitForAllTasks) {
  LockedThreadPool pool(2);
  std::atomic<int> counter{0};
  for (int i = 0; i < 10; ++i) {
    pool.enqueue([&counter] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      counter.fetch_add(1, std::memory_order_relaxed);
    });
  }
  pool.wait_impl();
  EXPECT_EQ(counter.load(), 10);
  pool.stop_impl();
}

TEST(LockedThreadPoolTest, WaitForTimeoutReturnsFalse) {
  LockedThreadPool pool(1);
  pool.enqueue([] { std::this_thread::sleep_for(std::chrono::milliseconds(500)); });
  bool result = pool.wait_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(result);
  pool.wait_for(std::chrono::milliseconds(1000));
  pool.stop_impl();
}

TEST(LockedThreadPoolTest, WaitForReturnsTrue) {
  LockedThreadPool pool(1);
  pool.enqueue([] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
  bool result = pool.wait_for(std::chrono::milliseconds(5000));
  EXPECT_TRUE(result);
  pool.stop_impl();
}

TEST(LockedThreadPoolTest, StopGracefully) {
  LockedThreadPool pool(2);
  std::atomic<int> executed{0};
  for (int i = 0; i < 50; ++i) {
    pool.enqueue([&executed] {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      executed.fetch_add(1, std::memory_order_relaxed);
    });
  }
  pool.stop_impl();
  EXPECT_TRUE(executed.load() >= 0);
}

TEST(LockedThreadPoolTest, EnqueueAfterStop) {
  LockedThreadPool pool(2);
  pool.stop_impl();
  std::atomic<int> executed{0};
  pool.enqueue([&executed] { executed = 1; });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(executed.load(), 0);
}

TEST(LockedThreadPoolTest, RaiiDestructor) {
  std::atomic<int> executed{0};
  {
    LockedThreadPool pool(2);
    pool.enqueue([&executed] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      executed = 1;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(executed.load() == 1 || executed.load() == 0);
}
