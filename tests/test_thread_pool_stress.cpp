#include "locked_thread_pool.h"
#include "thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <ctime>
#include <future>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace hps;

TEST(ThreadPoolStressTest, StopWithPendingTasks) {
  LockFreeThreadPool pool(2);
  std::atomic<int> completed{0};
  for (int i = 0; i < 10; ++i) {
    pool.enqueue([&completed] {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      completed.fetch_add(1, std::memory_order_relaxed);
    });
  }
  EXPECT_NO_THROW(pool.stop());
  EXPECT_EQ(completed.load(std::memory_order_relaxed), 10);
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

TEST(ThreadPoolStressTest, ConcurrentEnqueueStopDrainsPreviouslyAcceptedTasks) {
  constexpr int kPreAcceptedTaskCount = 64;
  constexpr int kConcurrentAttempts = 256;
  constexpr auto kSynchronizationTimeout = std::chrono::seconds(1);
  LockFreeThreadPool pool(1);
  std::atomic<int> preaccepted_completed{0};
  std::atomic<int> concurrent_attempts{0};
  std::latch worker_started(1);
  std::latch release_worker(1);

  pool.enqueue([&worker_started, &release_worker, &preaccepted_completed] {
    worker_started.count_down();
    release_worker.wait();
    preaccepted_completed.fetch_add(1, std::memory_order_relaxed);
  });
  worker_started.wait();

  for (int task_index = 0; task_index < kPreAcceptedTaskCount; ++task_index) {
    pool.enqueue([&preaccepted_completed] { preaccepted_completed.fetch_add(1, std::memory_order_relaxed); });
  }

  std::barrier start_race(3);
  std::promise<void> enqueue_started;
  auto enqueue_started_future = enqueue_started.get_future();
  std::promise<void> stop_started;
  auto stop_started_future = stop_started.get_future();

  std::thread enqueuer([&pool, &concurrent_attempts, &enqueue_started, &start_race] {
    start_race.arrive_and_wait();
    enqueue_started.set_value();
    for (int attempt = 0; attempt < kConcurrentAttempts; ++attempt) {
      pool.enqueue([] {});
      concurrent_attempts.fetch_add(1, std::memory_order_relaxed);
    }
  });
  std::thread stopper([&pool, &stop_started, &start_race] {
    start_race.arrive_and_wait();
    stop_started.set_value();
    pool.stop();
  });

  start_race.arrive_and_wait();
  const bool enqueue_entered_race = enqueue_started_future.wait_for(kSynchronizationTimeout) ==
                                    std::future_status::ready;
  const bool stop_entered_race = stop_started_future.wait_for(kSynchronizationTimeout) == std::future_status::ready;

  release_worker.count_down();
  enqueuer.join();
  stopper.join();

  EXPECT_TRUE(enqueue_entered_race);
  EXPECT_TRUE(stop_entered_race);
  EXPECT_EQ(concurrent_attempts.load(std::memory_order_relaxed), kConcurrentAttempts);
  EXPECT_EQ(preaccepted_completed.load(std::memory_order_relaxed), kPreAcceptedTaskCount + 1);
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

TEST(ThreadPoolStressTest, IdleWorkersDoNotBusyWait) {
  constexpr auto kIdleObservation = std::chrono::milliseconds(150);
  constexpr auto kMaximumIdleCpuTime = std::chrono::milliseconds(50);
  LockFreeThreadPool pool(2);

  struct timespec cpu_before {};

  ASSERT_EQ(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_before), 0);

  std::this_thread::sleep_for(kIdleObservation);

  struct timespec cpu_after {};

  ASSERT_EQ(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_after), 0);
  const auto to_duration = [](const struct timespec& timestamp) {
    return std::chrono::seconds(timestamp.tv_sec) + std::chrono::nanoseconds(timestamp.tv_nsec);
  };
  EXPECT_LT(to_duration(cpu_after) - to_duration(cpu_before), kMaximumIdleCpuTime);
  pool.stop();
}

TEST(ThreadPoolStressTest, IdleWorkerExecutesTaskAfterWake) {
  LockFreeThreadPool pool(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  std::promise<int> result;
  auto result_future = result.get_future();
  pool.enqueue([&result] { result.set_value(42); });

  EXPECT_EQ(result_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_EQ(result_future.get(), 42);
  pool.wait_for_all_tasks();
  pool.stop();
}

TEST(ThreadPoolStressTest, WaitForAllTasksDoesNotBusyWait) {
  constexpr auto kObservation = std::chrono::milliseconds(150);
  constexpr auto kMaximumWaitCpuTime = std::chrono::milliseconds(50);
  LockFreeThreadPool pool(1);
  std::promise<void> task_started;
  auto task_started_future = task_started.get_future();
  pool.enqueue([&task_started] {
    task_started.set_value();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
  });
  ASSERT_EQ(task_started_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);

  std::promise<void> wait_started;
  auto wait_started_future = wait_started.get_future();
  auto wait_future = std::async(std::launch::async, [&pool, &wait_started] {
    wait_started.set_value();
    pool.wait_for_all_tasks();
  });
  ASSERT_EQ(wait_started_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);

  struct timespec cpu_before {};

  ASSERT_EQ(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_before), 0);
  std::this_thread::sleep_for(kObservation);

  struct timespec cpu_after {};

  ASSERT_EQ(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_after), 0);
  const auto to_duration = [](const struct timespec& timestamp) {
    return std::chrono::seconds(timestamp.tv_sec) + std::chrono::nanoseconds(timestamp.tv_nsec);
  };
  EXPECT_LT(to_duration(cpu_after) - to_duration(cpu_before), kMaximumWaitCpuTime);
  EXPECT_EQ(wait_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  wait_future.get();
  pool.stop();
}

TEST(ThreadPoolStressTest, StopWakesWorkersAfterQueueBecomesIdle) {
  LockFreeThreadPool pool(2);
  std::latch queued_tasks_finished(2);
  for (int task_index = 0; task_index < 2; ++task_index) {
    pool.enqueue([&queued_tasks_finished] { queued_tasks_finished.count_down(); });
  }
  queued_tasks_finished.wait();
  pool.wait_for_all_tasks();

  auto stop_future = std::async(std::launch::async, [&pool] { pool.stop(); });
  ASSERT_EQ(stop_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  stop_future.get();
}

TEST(ThreadPoolStressTest, ManyWorkersDrainShortTaskBurst) {
  LockFreeThreadPool pool(32);
  constexpr int kTasks = 50000;
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
