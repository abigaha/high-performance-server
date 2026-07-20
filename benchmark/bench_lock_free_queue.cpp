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

static void BM_LFQ_MPMC_Contention(benchmark::State& state) {
  int num_producers = state.range(0);
  int num_consumers = state.range(1);
  int num_ops = state.range(2);
  for (auto _ : state) {
    hps::LockFreeQueue<int> q;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> done{false};

    std::vector<std::thread> producers;
    producers.reserve(static_cast<std::size_t>(num_producers));
    for (int t = 0; t < num_producers; ++t) {
      producers.emplace_back([&, t] {
        int start = t * num_ops;
        for (int i = 0; i < num_ops; ++i) {
          while (!q.push(start + i)) {
          }
          produced.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }

    std::vector<std::thread> consumers;
    consumers.reserve(static_cast<std::size_t>(num_consumers));
    int total = num_producers * num_ops;
    for (int t = 0; t < num_consumers; ++t) {
      consumers.emplace_back([&] {
        int val = 0;
        while (consumed.load(std::memory_order_relaxed) < total) {
          if (q.pop(val)) {
            consumed.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();
    benchmark::DoNotOptimize(produced.load());
    benchmark::DoNotOptimize(consumed.load());
  }
  state.SetItemsProcessed(state.iterations() * num_producers * num_ops);
}

BENCHMARK(BM_LFQ_MPMC_Contention)->Args({2, 2, 10000})->Args({4, 4, 5000});

static void BM_LFQ_HighContention(benchmark::State& state) {
  int num_threads = state.range(0);
  int num_ops = state.range(1);
  for (auto _ : state) {
    hps::LockFreeQueue<int> q;
    std::atomic<int> total_ops{0};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(num_threads));
    for (int t = 0; t < num_threads; ++t) {
      threads.emplace_back([&, t] {
        for (int i = 0; i < num_ops; ++i) {
          if (t % 2 == 0) {
            while (!q.push(t * num_ops + i)) {
            }
          } else {
            int val = 0;
            while (!q.pop(val)) {
            }
          }
          total_ops.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    for (auto& t : threads) t.join();
    benchmark::DoNotOptimize(total_ops.load());
  }
  state.SetItemsProcessed(state.iterations() * num_threads * num_ops);
}

BENCHMARK(BM_LFQ_HighContention)->Args({8, 5000})->Args({16, 2000});

BENCHMARK_MAIN();
