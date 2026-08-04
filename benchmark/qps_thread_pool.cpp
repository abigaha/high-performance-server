#include "locked_thread_pool.h"
#include "qps_runner.hpp"
#include "thread_pool.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <semaphore>

namespace {

void bench_pool(hps::bench::QpsSample& sample, auto& pool, int concurrency, int duration_sec) {
  constexpr std::ptrdiff_t kMaxInFlightTasks = 1024;
  std::atomic<int64_t> completed{0};
  std::atomic<bool> start_flag{false};
  std::counting_semaphore<kMaxInFlightTasks> available_slots{kMaxInFlightTasks};
  std::vector<std::thread> threads;

  for (int i = 0; i < concurrency; ++i) {
    threads.emplace_back([&, i]() {
      while (!start_flag.load(std::memory_order_acquire)) {
      }
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
      while (std::chrono::steady_clock::now() < deadline) {
        available_slots.acquire();
        pool.enqueue([&completed, &available_slots] {
          completed.fetch_add(1, std::memory_order_relaxed);
          available_slots.release();
        });
      }
    });
  }

  auto wall_start = std::chrono::steady_clock::now();
  start_flag.store(true, std::memory_order_release);

  // 心跳
  std::atomic<bool> heartbeat_stop{false};
  std::thread heartbeat([&] {
    int elapsed = 0;
    while (elapsed < duration_sec && !heartbeat_stop.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      if (!heartbeat_stop.load()) {
        std::cout << "." << std::flush;
      }
      elapsed += 5;
    }
  });

  // 周期唤醒等待测试结束，避免高并发下主线程 sleep_until 单次唤醒抢不到 CPU
  {
    auto deadline = wall_start + std::chrono::seconds(duration_sec) + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  heartbeat_stop.store(true);
  if (heartbeat.joinable())
    heartbeat.join();

  // Stop producers before stopping the pool.  Otherwise stop() can close the
  // worker side while a producer is still finishing an enqueue operation.
  for (auto& t : threads) t.join();
  pool.wait_for_all_tasks();
  pool.stop();
  pool.wait_for_all_tasks();
  auto wall_end = std::chrono::steady_clock::now();

  double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();
  int64_t total = completed.load();

  sample.concurrency = concurrency;
  sample.total_ops = total;
  sample.qps = wall_sec > 0 ? static_cast<double>(total) / wall_sec : 0.0;
}

} // namespace

int main() noexcept {
  try {
    auto levels = hps::bench::default_qps_levels();
    std::vector<hps::bench::QpsSample> results;

    // LockFreeThreadPool
    {
      results.clear();
      for (auto& lv : levels) {
        hps::LockFreeThreadPool pool(4);
        hps::bench::QpsSample s;
        std::cout << std::format("  [LockFreeTP 并发={:>4} 时长={:>2}s]", lv.concurrency, lv.duration_sec);
        std::cout.flush();
        bench_pool(s, pool, lv.concurrency, lv.duration_sec);
        pool.stop();
        std::cout << std::format(" QPS={:>10}\n", static_cast<int64_t>(s.qps));
        results.push_back(s);
      }
      hps::bench::print_qps_report("LockFreeThreadPool Enqueue", results);
    }

    // LockedThreadPool
    {
      results.clear();
      for (auto& lv : levels) {
        hps::LockedThreadPool pool(4);
        hps::bench::QpsSample s;
        std::cout << std::format("  [LockedTP   并发={:>4} 时长={:>2}s]", lv.concurrency, lv.duration_sec);
        std::cout.flush();
        bench_pool(s, pool, lv.concurrency, lv.duration_sec);
        pool.stop();
        std::cout << std::format(" QPS={:>10}\n", static_cast<int64_t>(s.qps));
        results.push_back(s);
      }
      hps::bench::print_qps_report("LockedThreadPool Enqueue", results);
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "线程池 QPS 基准失败: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "线程池 QPS 基准失败: 未知异常\n";
  }
  return EXIT_FAILURE;
}
