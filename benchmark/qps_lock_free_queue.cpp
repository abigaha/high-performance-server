#include "lock_free_queue.hpp"
#include "qps_runner.hpp"

#include <atomic>
#include <cstddef>
#include <format>
#include <latch>
#include <thread>
#include <vector>

int main() {
  auto levels = hps::bench::default_qps_levels();
  std::vector<hps::bench::QpsSample> results;

  for (auto& lv : levels) {
    int c = lv.concurrency;
    int duration = lv.duration_sec;
    std::cout << std::format("  [LockFreeQueue MPMC 并发={:>4} 时长={:>2}s]", c, duration);
    std::cout.flush();

    hps::LockFreeQueue<int, 65535> q;
    std::atomic<int64_t> total_ops{0};
    std::atomic<bool> start_flag{false};
    std::atomic<bool> stop_flag{false};
    std::latch done{2 * c};

    std::vector<std::thread> threads;
    threads.reserve(c * 2);

    for (int i = 0; i < c; ++i) {
      threads.emplace_back([&, i]() {
        while (!start_flag.load(std::memory_order_acquire)) {
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
        int64_t local_ops = 0;
        while (std::chrono::steady_clock::now() < deadline && !stop_flag.load()) {
          if (q.push(i)) {
            ++local_ops;
          } else {
            std::this_thread::yield();
          }
        }
        total_ops.fetch_add(local_ops, std::memory_order_relaxed);
        done.count_down();
      });

      threads.emplace_back([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
        }
        int val = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
        while (std::chrono::steady_clock::now() < deadline && !stop_flag.load()) {
          if (!q.try_pop(val)) {
            std::this_thread::yield();
          }
        }
        done.count_down();
      });
    }

    auto wall_start = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);

    std::atomic<bool> heartbeat_stop{false};
    std::thread heartbeat([&] {
      int elapsed = 0;
      while (elapsed < duration && !heartbeat_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!heartbeat_stop.load()) {
          std::cout << "." << std::flush;
        }
        elapsed += 5;
      }
    });

    auto deadline = wall_start + std::chrono::seconds(duration) + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    stop_flag.store(true, std::memory_order_release);
    q.stop();

    // 等待所有 worker 线程退出（最多等 30s）
    if (!done.try_wait()) {
      done.wait();
    }

    heartbeat_stop.store(true);
    if (heartbeat.joinable())
      heartbeat.join();
    for (auto& t : threads) t.join();

    auto wall_end = std::chrono::steady_clock::now();
    double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();
    int64_t total = total_ops.load();

    hps::bench::QpsSample s;
    s.concurrency = c;
    s.total_ops = total;
    s.qps = wall_sec > 0 ? static_cast<double>(total) / wall_sec : 0.0;
    std::cout << std::format(" QPS={:>10}\n", static_cast<int64_t>(s.qps));
    results.push_back(s);
  }
  hps::bench::print_qps_report("LockFreeQueue MPMC", results);

  return 0;
}
