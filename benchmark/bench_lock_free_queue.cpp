#include "lock_free_queue.hpp"
#include "move_only_function.h"

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace {

template <typename T>
void spsc_bench(benchmark::State& state, int num_ops) {
  for (auto _ : state) {
    hps::LockFreeQueue<T> q;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    std::thread producer([&] {
      for (int i = 0; i < num_ops; ++i) {
        T val{};
        while (!q.push(std::move(val))) {
        }
        produced.fetch_add(1, std::memory_order_relaxed);
      }
    });

    std::thread consumer([&] {
      T val;
      int count = 0;
      while (count < num_ops) {
        if (q.pop(val)) {
          ++count;
          consumed.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });

    producer.join();
    consumer.join();
    benchmark::DoNotOptimize(produced.load());
    benchmark::DoNotOptimize(consumed.load());
  }
  state.SetItemsProcessed(state.iterations() * num_ops);
}

} // namespace

static void BM_LockFreeQueue_SPSC_1k(benchmark::State& state) {
  spsc_bench<int>(state, 10000);
}

BENCHMARK(BM_LockFreeQueue_SPSC_1k);

static void BM_LockFreeQueue_SPSC_10k(benchmark::State& state) {
  spsc_bench<int>(state, 100000);
}

BENCHMARK(BM_LockFreeQueue_SPSC_10k);

BENCHMARK_MAIN();
