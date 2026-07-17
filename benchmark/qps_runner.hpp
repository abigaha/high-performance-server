#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace hps::bench {

struct QpsSample {
  int concurrency = 0;
  int duration_sec = 0;
  int64_t total_ops = 0;
  double qps = 0.0;
  double avg_latency_ns = 0.0;
  double p50_ns = 0.0;
  double p90_ns = 0.0;
  double p99_ns = 0.0;
  double p99_9_ns = 0.0;
};

struct QpsLevel {
  int concurrency;
  int duration_sec;
};

static constexpr size_t kSampleInterval = 32;

inline std::vector<QpsLevel> default_qps_levels() {
  return {
    {1, 5},
    {2, 5},
    {4, 5},
    {8, 5},
    {16, 5},
    {32, 5},
    {64, 5},
    {128, 5},
    {256, 15},
    {512, 15},
    {1024, 15},
  };
}

template <typename Func>
QpsSample run_qps(int concurrency, int duration_sec, Func&& op) {
  struct ThreadResult {
    int64_t ops = 0;
    std::vector<int64_t> latencies;
  };

  std::vector<std::thread> threads;
  std::vector<ThreadResult> results(concurrency);
  std::atomic<bool> start_flag{false};

  auto thread_main = [&](int tid) {
    while (!start_flag.load(std::memory_order_acquire)) {
    }

    auto& local = results[tid];
    local.ops = 0;
    local.latencies.reserve(static_cast<size_t>(duration_sec) * 1000000 / kSampleInterval);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
    size_t sample_counter = 0;

    while (std::chrono::steady_clock::now() < deadline) {
      auto t0 = std::chrono::steady_clock::now();
      op(tid);
      auto t1 = std::chrono::steady_clock::now();
      ++local.ops;

      if (++sample_counter >= kSampleInterval) {
        sample_counter = 0;
        local.latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      }
    }
  };

  for (int i = 0; i < concurrency; ++i) {
    threads.emplace_back(thread_main, i);
  }

  auto wall_start = std::chrono::steady_clock::now();
  start_flag.store(true, std::memory_order_release);

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

  for (auto& t : threads) {
    t.join();
  }
  heartbeat_stop.store(true);
  if (heartbeat.joinable())
    heartbeat.join();
  auto wall_end = std::chrono::steady_clock::now();

  double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();
  int64_t total_ops = 0;
  for (auto& r : results) {
    total_ops += r.ops;
  }

  std::vector<int64_t> all_lat;
  size_t total_s = 0;
  for (auto& r : results) {
    total_s += r.latencies.size();
  }
  all_lat.reserve(total_s);
  for (auto& r : results) {
    all_lat.insert(all_lat.end(), r.latencies.begin(), r.latencies.end());
  }
  std::sort(all_lat.begin(), all_lat.end());

  QpsSample s;
  s.concurrency = concurrency;
  s.duration_sec = duration_sec;
  s.total_ops = total_ops;
  s.qps = wall_sec > 0 ? static_cast<double>(total_ops) / wall_sec : 0.0;
  if (!all_lat.empty()) {
    double sum = 0;
    for (auto v : all_lat) sum += v;
    s.avg_latency_ns = sum / static_cast<double>(all_lat.size());
    s.p50_ns = static_cast<double>(all_lat[static_cast<size_t>(static_cast<double>(all_lat.size()) * 0.50)]);
    s.p90_ns = static_cast<double>(all_lat[static_cast<size_t>(static_cast<double>(all_lat.size()) * 0.90)]);
    s.p99_ns = static_cast<double>(all_lat[static_cast<size_t>(static_cast<double>(all_lat.size()) * 0.99)]);
    s.p99_9_ns = static_cast<double>(all_lat[static_cast<size_t>(static_cast<double>(all_lat.size()) * 0.999)]);
  }
  return s;
}

inline void print_qps_header() {
  std::cout << std::format("{:<8} {:>12} {:>12} {:>12} {:>12} {:>12} {:>12} {:>12}\n",
                           "并发",
                           "总操作数",
                           "QPS",
                           "平均(ns)",
                           "P50(ns)",
                           "P90(ns)",
                           "P99(ns)",
                           "P99.9(ns)");
  std::cout << std::format("{:<8} {:>12} {:>12} {:>12} {:>12} {:>12} {:>12} {:>12}\n",
                           "------",
                           "----------",
                           "---",
                           "--------",
                           "-------",
                           "-------",
                           "-------",
                           "---------");
}

inline void print_qps_row(const QpsSample& s) {
  std::cout << std::format("{:<8} {:>12} {:>12.0f} {:>12.0f} {:>12.0f} {:>12.0f} {:>12.0f} {:>12.0f}\n",
                           s.concurrency,
                           s.total_ops,
                           s.qps,
                           s.avg_latency_ns,
                           s.p50_ns,
                           s.p90_ns,
                           s.p99_ns,
                           s.p99_9_ns);
}

inline void print_qps_report(const std::string& bench_name, const std::vector<QpsSample>& results) {
  std::cout << std::format("\n=== {} ===\n", bench_name);
  bool has_latency = false;
  for (auto& s : results) {
    if (s.p50_ns > 0) {
      has_latency = true;
      break;
    }
  }
  if (has_latency) {
    print_qps_header();
    for (auto& s : results) {
      print_qps_row(s);
    }
  } else {
    std::cout << std::format("{:<8} {:>12} {:>12}\n", "并发", "总操作数", "QPS");
    std::cout << std::format("{:<8} {:>12} {:>12}\n", "------", "----------", "---");
    for (auto& s : results) {
      std::cout << std::format("{:<8} {:>12} {:>12.0f}\n", s.concurrency, s.total_ops, s.qps);
    }
    std::cout << "注: 延迟数据缺失 — 本基准以 fire-and-forget 方式提交任务，"
              << "仅统计入队吞吐量，未测量任务从提交到执行完毕的完整延迟。\n";
  }
}

template <typename Func>
void run_qps_steps(const std::string& bench_name, const std::vector<QpsLevel>& levels, Func&& op) {
  std::vector<QpsSample> all_results;
  for (auto& lv : levels) {
    std::cout << std::format("  [并发={:>4} 时长={:>2}s]", lv.concurrency, lv.duration_sec);
    std::cout.flush();
    auto sample = run_qps(lv.concurrency, lv.duration_sec, op);
    std::cout << std::format(" QPS={:>10}\n", static_cast<long>(sample.qps));
    all_results.push_back(sample);
  }
  print_qps_report(bench_name, all_results);
}

} // namespace hps::bench
