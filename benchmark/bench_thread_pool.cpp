#include "locked_thread_pool.h"
#include "thread_pool.h"

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace {

void do_work(::benchmark::State& state, auto& pool) {
  std::atomic<int> counter{0};
  int total = static_cast<int>(state.range(0));
  for (auto _ : state) {
    counter.store(0);
    for (int i = 0; i < total; ++i) {
      pool.enqueue([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.wait_for_all_tasks();
    benchmark::DoNotOptimize(counter.load());
  }
  state.SetItemsProcessed(state.iterations() * total);
}

} // namespace

static void BM_LockFreeThreadPool_Tasks(benchmark::State& state) {
  hps::LockFreeThreadPool pool(4);
  do_work(state, pool);
  pool.stop();
}

BENCHMARK(BM_LockFreeThreadPool_Tasks)->Arg(100)->Arg(1000);

static void BM_LockedThreadPool_Tasks(benchmark::State& state) {
  hps::LockedThreadPool pool(4);
  do_work(state, pool);
  pool.stop();
}

BENCHMARK(BM_LockedThreadPool_Tasks)->Arg(100)->Arg(1000);

static void BM_LockFreeThreadPool_HeavyTask(benchmark::State& state) {
  hps::LockFreeThreadPool pool(4);
  for (auto _ : state) {
    std::atomic<int> done{0};
    for (int i = 0; i < 1000; ++i) {
      pool.enqueue([&done] {
        volatile int sum = 0;
        for (int j = 0; j < 100; ++j) sum += j;
        benchmark::DoNotOptimize(sum);
        done.fetch_add(1, std::memory_order_relaxed);
      });
    }
    pool.wait_for_all_tasks();
    benchmark::DoNotOptimize(done.load());
  }
  pool.stop();
}

BENCHMARK(BM_LockFreeThreadPool_HeavyTask);

static void BM_LockedThreadPool_HeavyTask(benchmark::State& state) {
  hps::LockedThreadPool pool(4);
  for (auto _ : state) {
    std::atomic<int> done{0};
    for (int i = 0; i < 1000; ++i) {
      pool.enqueue([&done] {
        volatile int sum = 0;
        for (int j = 0; j < 100; ++j) sum += j;
        benchmark::DoNotOptimize(sum);
        done.fetch_add(1, std::memory_order_relaxed);
      });
    }
    pool.wait_for_all_tasks();
    benchmark::DoNotOptimize(done.load());
  }
  pool.stop();
}

BENCHMARK(BM_LockedThreadPool_HeavyTask);

static void BM_LockedThreadPool_EnqueueDelay(benchmark::State& state) {
  int total = state.range(0);
  hps::LockedThreadPool pool(4);
  std::vector<int64_t> latencies;
  latencies.reserve(static_cast<std::size_t>(total));
  for (auto _ : state) {
    latencies.clear();
    for (int i = 0; i < total; ++i) {
      auto t0 = std::chrono::steady_clock::now();
      pool.enqueue([] {});
      auto t1 = std::chrono::steady_clock::now();
      latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    pool.wait_for_all_tasks();
    std::sort(latencies.begin(), latencies.end());
    benchmark::DoNotOptimize(latencies[latencies.size() / 2]);
  }
  pool.stop();
  state.SetItemsProcessed(state.iterations() * total);
}

BENCHMARK(BM_LockedThreadPool_EnqueueDelay)->Arg(1000)->Arg(10000);

BENCHMARK_MAIN();
