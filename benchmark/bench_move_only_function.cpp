#include "move_only_function.h"

#include <benchmark/benchmark.h>

#include <functional>
#include <string>
#include <utility>

namespace {

struct BigObject {
  char data[256] = {};
  int value = 42;
};

} // namespace

static void BM_MoveOnlyFunction_ConstructVoid(benchmark::State& state) {
  for (auto _ : state) {
    hps::MoveOnlyFunction f([]() {});
    benchmark::DoNotOptimize(f);
  }
}

BENCHMARK(BM_MoveOnlyFunction_ConstructVoid);

static void BM_MoveOnlyFunction_ConstructWithCapture(benchmark::State& state) {
  for (auto _ : state) {
    int x = 42;
    hps::MoveOnlyFunction f([x]() { (void)x; });
    benchmark::DoNotOptimize(f);
  }
}

BENCHMARK(BM_MoveOnlyFunction_ConstructWithCapture);

static void BM_MoveOnlyFunction_ConstructLargeCapture(benchmark::State& state) {
  for (auto _ : state) {
    BigObject obj;
    hps::MoveOnlyFunction f([obj]() { (void)obj.value; });
    benchmark::DoNotOptimize(f);
  }
}

BENCHMARK(BM_MoveOnlyFunction_ConstructLargeCapture);

static void BM_MoveOnlyFunction_Invoke(benchmark::State& state) {
  int result = 0;
  hps::MoveOnlyFunction f([&result]() { result = 42; });
  for (auto _ : state) {
    f();
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK(BM_MoveOnlyFunction_Invoke);

static void BM_MoveOnlyFunction_MoveConstruct(benchmark::State& state) {
  for (auto _ : state) {
    hps::MoveOnlyFunction f1([]() {});
    hps::MoveOnlyFunction f2(std::move(f1));
    benchmark::DoNotOptimize(f2);
  }
}

BENCHMARK(BM_MoveOnlyFunction_MoveConstruct);

static void BM_StdFunction_Compare(benchmark::State& state) {
  for (auto _ : state) {
    std::function<void()> f([]() {});
    benchmark::DoNotOptimize(f);
  }
}

BENCHMARK(BM_StdFunction_Compare);

BENCHMARK_MAIN();
