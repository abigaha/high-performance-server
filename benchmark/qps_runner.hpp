#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <iostream>
#include <latch>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace hps::bench {

struct QpsSample {
  int concurrency = 0;
  int duration_sec = 0;
  int64_t total_attempts = 0;
  // total_ops 表示成功操作数，保留名称以兼容手写 QPS 场景。
  int64_t total_ops = 0;
  int64_t total_errors = 0;
  std::size_t latency_samples = 0;
  double qps = 0.0;
  double error_rate = 0.0;
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

inline constexpr std::size_t kSampleInterval = 32;
inline constexpr std::size_t kMaxLatencySamplesPerThread = 4096;

inline std::vector<QpsLevel> default_qps_levels() {
  const char* profile_env = std::getenv("QPS_PROFILE");
  const std::string_view profile = profile_env == nullptr ? "full" : std::string_view(profile_env);
  if (profile.empty() || profile == "full") {
    return {
      {1, 5},
      {4, 5},
      {16, 5},
      {64, 5},
      {256, 15},
      {512, 15},
      {1024, 15},
    };
  }
  if (profile == "smoke") {
    return {
      {1, 1},
      {4, 1},
    };
  }
  throw std::invalid_argument("QPS_PROFILE 仅支持 smoke 或 full");
}

namespace detail {

template <typename Func>
bool invoke_qps_operation(Func& op, int tid) {
  using Result = std::invoke_result_t<Func&, int>;
  if constexpr (std::is_void_v<Result>) {
    std::invoke(op, tid);
    return true;
  } else {
    static_assert(std::is_convertible_v<Result, bool>, "QPS 操作必须返回 void 或可转换为 bool 的结果");
    return static_cast<bool>(std::invoke(op, tid));
  }
}

inline std::size_t percentile_index(std::size_t size, double percentile) {
  return static_cast<std::size_t>(static_cast<double>(size - 1) * percentile);
}

inline void add_latency_sample(std::vector<int64_t>& samples,
                               std::uint64_t& samples_seen,
                               std::uint64_t& random_state,
                               int64_t latency_ns) {
  ++samples_seen;
  if (samples.size() < kMaxLatencySamplesPerThread) {
    samples.push_back(latency_ns);
    return;
  }

  // xorshift64 用于 reservoir 替换；每 32 次操作才执行一次，不污染主要热路径。
  random_state ^= random_state << 13U;
  random_state ^= random_state >> 7U;
  random_state ^= random_state << 17U;
  const auto candidate = static_cast<std::size_t>(random_state % samples_seen);
  if (candidate < samples.size()) {
    samples[candidate] = latency_ns;
  }
}

} // namespace detail

template <typename Func>
QpsSample run_qps(int concurrency, int duration_sec, Func&& op) {
  if (concurrency <= 0 || duration_sec <= 0) {
    throw std::invalid_argument("QPS 并发数和持续时间必须大于 0");
  }

  struct ThreadResult {
    int64_t attempts = 0;
    int64_t successful_ops = 0;
    int64_t errors = 0;
    std::vector<int64_t> latencies;
  };

  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(concurrency));
  std::vector<ThreadResult> results(static_cast<std::size_t>(concurrency));
  std::atomic<bool> start_flag{false};
  std::latch ready_latch(concurrency);
  std::chrono::steady_clock::time_point deadline;

  auto thread_main = [&](int tid) {
    auto& local = results[static_cast<std::size_t>(tid)];
    local.latencies.reserve(kMaxLatencySamplesPerThread);
    std::uint64_t samples_seen = 0;
    std::uint64_t random_state = 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint64_t>(tid + 1);
    std::size_t sample_counter = 0;

    ready_latch.count_down();
    while (!start_flag.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    while (std::chrono::steady_clock::now() < deadline) {
      const auto t0 = std::chrono::steady_clock::now();
      bool succeeded = false;
      try {
        succeeded = detail::invoke_qps_operation(op, tid);
      } catch (...) {
        succeeded = false;
      }
      const auto t1 = std::chrono::steady_clock::now();

      ++local.attempts;
      if (succeeded) {
        ++local.successful_ops;
        if (++sample_counter >= kSampleInterval) {
          sample_counter = 0;
          detail::add_latency_sample(local.latencies,
                                     samples_seen,
                                     random_state,
                                     std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
      } else {
        ++local.errors;
      }
    }
  };

  try {
    for (int i = 0; i < concurrency; ++i) {
      threads.emplace_back(thread_main, i);
    }
  } catch (...) {
    deadline = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);
    for (auto& thread : threads) {
      thread.join();
    }
    throw;
  }

  ready_latch.wait();
  std::mutex heartbeat_mutex;
  std::condition_variable heartbeat_cv;
  bool heartbeat_stop = false;
  std::thread heartbeat;
  try {
    heartbeat = std::thread([&] {
      std::unique_lock lock(heartbeat_mutex);
      while (!heartbeat_stop) {
        if (heartbeat_cv.wait_for(lock, std::chrono::seconds(5), [&] { return heartbeat_stop; })) {
          break;
        }
        std::cout << "." << std::flush;
      }
    });
  } catch (...) {
    deadline = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);
    for (auto& thread : threads) {
      thread.join();
    }
    throw;
  }

  const auto wall_start = std::chrono::steady_clock::now();
  deadline = wall_start + std::chrono::seconds(duration_sec);
  start_flag.store(true, std::memory_order_release);

  for (auto& thread : threads) {
    thread.join();
  }
  {
    std::lock_guard lock(heartbeat_mutex);
    heartbeat_stop = true;
  }
  heartbeat_cv.notify_one();
  heartbeat.join();
  const auto wall_end = std::chrono::steady_clock::now();

  int64_t total_attempts = 0;
  int64_t total_ops = 0;
  int64_t total_errors = 0;
  std::size_t total_samples = 0;
  for (const auto& result : results) {
    total_attempts += result.attempts;
    total_ops += result.successful_ops;
    total_errors += result.errors;
    total_samples += result.latencies.size();
  }

  std::vector<int64_t> all_latencies;
  all_latencies.reserve(total_samples);
  for (const auto& result : results) {
    all_latencies.insert(all_latencies.end(), result.latencies.begin(), result.latencies.end());
  }
  std::sort(all_latencies.begin(), all_latencies.end());

  const double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();
  QpsSample sample;
  sample.concurrency = concurrency;
  sample.duration_sec = duration_sec;
  sample.total_attempts = total_attempts;
  sample.total_ops = total_ops;
  sample.total_errors = total_errors;
  sample.latency_samples = all_latencies.size();
  sample.qps = wall_sec > 0.0 ? static_cast<double>(total_ops) / wall_sec : 0.0;
  sample.error_rate = total_attempts > 0 ? static_cast<double>(total_errors) * 100.0 / total_attempts : 0.0;
  if (!all_latencies.empty()) {
    const double sum = std::accumulate(all_latencies.begin(), all_latencies.end(), 0.0);
    sample.avg_latency_ns = sum / static_cast<double>(all_latencies.size());
    sample.p50_ns = static_cast<double>(all_latencies[detail::percentile_index(all_latencies.size(), 0.50)]);
    sample.p90_ns = static_cast<double>(all_latencies[detail::percentile_index(all_latencies.size(), 0.90)]);
    sample.p99_ns = static_cast<double>(all_latencies[detail::percentile_index(all_latencies.size(), 0.99)]);
    sample.p99_9_ns = static_cast<double>(all_latencies[detail::percentile_index(all_latencies.size(), 0.999)]);
  }
  return sample;
}

inline void print_qps_header() {
  std::cout << std::format("{:<8} {:>12} {:>12} {:>12} {:>10} {:>12} {:>12} {:>12} {:>12} {:>12} {:>12}\n",
                           "并发",
                           "尝试数",
                           "成功数",
                           "错误数",
                           "错误率(%)",
                           "QPS",
                           "平均(ns)",
                           "P50(ns)",
                           "P90(ns)",
                           "P99(ns)",
                           "P99.9(ns)");
  std::cout << std::format("{:<8} {:>12} {:>12} {:>12} {:>10} {:>12} {:>12} {:>12} {:>12} {:>12} {:>12}\n",
                           "------",
                           "----------",
                           "----------",
                           "----------",
                           "----------",
                           "---",
                           "--------",
                           "-------",
                           "-------",
                           "-------",
                           "---------");
}

inline void print_qps_row(const QpsSample& s) {
  const auto attempts = s.total_attempts > 0 ? s.total_attempts : s.total_ops;
  std::cout << std::format(
    "{:<8} {:>12} {:>12} {:>12} {:>10.4f} {:>12.0f} {:>12.0f} {:>12.0f} {:>12.0f} {:>12.0f} {:>12.0f}\n",
    s.concurrency,
    attempts,
    s.total_ops,
    s.total_errors,
    s.error_rate,
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
  int64_t measured_attempts = 0;
  int64_t successful_ops = 0;
  for (const auto& s : results) {
    measured_attempts += s.total_attempts;
    successful_ops += s.total_ops;
    if (s.latency_samples > 0 || s.p50_ns > 0) {
      has_latency = true;
    }
  }
  if (has_latency) {
    print_qps_header();
    for (const auto& s : results) {
      print_qps_row(s);
    }
  } else {
    std::cout << std::format(
      "{:<8} {:>12} {:>12} {:>12} {:>10} {:>12}\n", "并发", "尝试数", "成功数", "错误数", "错误率(%)", "QPS");
    std::cout << std::format("{:<8} {:>12} {:>12} {:>12} {:>10} {:>12}\n",
                             "------",
                             "----------",
                             "----------",
                             "----------",
                             "----------",
                             "---");
    for (const auto& s : results) {
      const auto attempts = s.total_attempts > 0 ? s.total_attempts : s.total_ops;
      std::cout << std::format("{:<8} {:>12} {:>12} {:>12} {:>10.4f} {:>12.0f}\n",
                               s.concurrency,
                               attempts,
                               s.total_ops,
                               s.total_errors,
                               s.error_rate,
                               s.qps);
    }
    if (measured_attempts == 0) {
      std::cout << "注: 延迟数据缺失 — 本基准以 fire-and-forget 方式提交任务，"
                << "仅统计入队吞吐量，未测量任务从提交到执行完毕的完整延迟。\n";
    } else if (successful_ops == 0) {
      std::cout << "注: 无成功操作，未生成成功延迟样本。\n";
    } else {
      std::cout << "注: 成功操作未达到按线程采样间隔，未生成成功延迟样本。\n";
    }
  }
}

template <typename Func>
void run_qps_steps(const std::string& bench_name, const std::vector<QpsLevel>& levels, Func&& op) {
  std::vector<QpsSample> all_results;
  all_results.reserve(levels.size());
  int64_t total_errors = 0;
  for (const auto& lv : levels) {
    std::cout << std::format("  [并发={:>4} 时长={:>2}s]", lv.concurrency, lv.duration_sec);
    std::cout.flush();
    auto sample = run_qps(lv.concurrency, lv.duration_sec, op);
    std::cout << std::format(" QPS={:>10} 错误={:>8}\n", static_cast<int64_t>(sample.qps), sample.total_errors);
    total_errors += sample.total_errors;
    all_results.push_back(sample);
  }
  print_qps_report(bench_name, all_results);
  if (total_errors > 0) {
    throw std::runtime_error(std::format("{} 检测到 {} 次失败操作", bench_name, total_errors));
  }
}

} // namespace hps::bench
