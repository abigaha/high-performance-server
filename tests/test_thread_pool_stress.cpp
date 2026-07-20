#include "locked_thread_pool.h"
#include "thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace hps;

TEST(ThreadPoolStressTest, StopWithPendingTasks) {
  LockFreeThreadPool pool(2);
  for (int i = 0; i < 10; ++i) {
    pool.enqueue([] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
  }
  EXPECT_NO_THROW(pool.stop());
}

TEST(ThreadPoolStressTest, ExceptionInTask) {
  LockFreeThreadPool pool(2);
  std::atomic<bool> exception_caught{false};
  // 异常在 worker 线程内抛出，需在任务内部捕获避免 std::terminate
  pool.enqueue([&exception_caught] {
    try {
      throw std::runtime_error("task error");
    } catch (...) {
      exception_caught.store(true);
    }
  });
  pool.wait_for_all_tasks();
  EXPECT_TRUE(exception_caught.load());
  pool.stop();
}

TEST(ThreadPoolStressTest, ConcurrentEnqueueStop) {
  LockFreeThreadPool pool(4);
  std::atomic<bool> running{true};
  std::vector<std::thread> enqueuers;
  for (int i = 0; i < 4; ++i) {
    enqueuers.emplace_back([&pool, &running]() {
      while (running.load()) {
        pool.enqueue([]() { std::this_thread::yield(); });
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  running.store(false);
  pool.stop();
  for (auto& t : enqueuers) {
    t.join();
  }
}

TEST(ThreadPoolStressTest, WaitForAllTasks) {
  LockFreeThreadPool pool(4);
  std::atomic<int> counter{0};
  constexpr int kTasks = 100;
  for (int i = 0; i < kTasks; ++i) {
    pool.enqueue([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  }
  pool.wait_for_all_tasks();
  EXPECT_EQ(counter.load(), kTasks);
  pool.stop();
}

TEST(ThreadPoolStressTest, EmptyPoolWait) {
  LockFreeThreadPool pool(2);
  EXPECT_NO_THROW(pool.wait_for_all_tasks());
  pool.stop();
}

TEST(ThreadPoolStressTest, ManyShortTasks) {
  LockFreeThreadPool pool(4);
  constexpr int kTasks = 10000;
  std::atomic<int> counter{0};
  for (int i = 0; i < kTasks; ++i) {
    pool.enqueue([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  }
  pool.wait_for_all_tasks();
  EXPECT_EQ(counter.load(), kTasks);
  pool.stop();
}

TEST(ThreadPoolStressTest, TaskWithPromise) {
  LockFreeThreadPool pool(2);
  std::promise<int> prom;
  auto fut = prom.get_future();
  pool.enqueue([&prom] { prom.set_value(42); });
  pool.wait_for_all_tasks();
  EXPECT_EQ(fut.get(), 42);
  pool.stop();
}

TEST(ThreadPoolStressTest, LockedThreadPoolBasic) {
  LockedThreadPool pool(4);
  std::atomic<int> counter{0};
  constexpr int kTasks = 100;
  for (int i = 0; i < kTasks; ++i) {
    pool.enqueue([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  }
  pool.wait_impl();
  EXPECT_EQ(counter.load(), kTasks);
  pool.stop_impl();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
