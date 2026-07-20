#include "file_system.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string create_temp_file(std::size_t size) {
  char path[] = "/tmp/hps_bench_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    return "";
  std::vector<char> buf(size, 'a');
  ssize_t written = 0;
  while (written < static_cast<ssize_t>(size)) {
    ssize_t n = write(fd, buf.data() + written, size - static_cast<std::size_t>(written));
    if (n < 0)
      break;
    written += n;
  }
  close(fd);
  return std::string(path);
}

} // namespace

static void BM_FileSystem_Sha256(benchmark::State& state) {
  std::size_t size = static_cast<std::size_t>(state.range(0));
  auto path = create_temp_file(size);
  if (path.empty()) {
    state.SkipWithError("failed to create temp file");
    return;
  }
  hps::FileSystem fs("/tmp");
  for (auto _ : state) {
    auto hash = fs.compute_file_hash(path);
    benchmark::DoNotOptimize(hash);
  }
  state.SetBytesProcessed(state.iterations() * size);
  std::remove(path.c_str());
}

BENCHMARK(BM_FileSystem_Sha256)->Arg(1024)->Arg(65536)->Arg(1048576);

static void BM_FileSystem_Sha256SmallData(benchmark::State& state) {
  std::string data(static_cast<std::size_t>(state.range(0)), 'x');
  for (auto _ : state) {
    auto hash = hps::FileSystem::sha256_hex(data.data(), data.size());
    benchmark::DoNotOptimize(hash);
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_FileSystem_Sha256SmallData)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(8192);

static void BM_FileSystem_SplitFile(benchmark::State& state) {
  std::size_t file_size = static_cast<std::size_t>(state.range(0));
  auto path = create_temp_file(file_size);
  if (path.empty()) {
    state.SkipWithError("failed to create temp file");
    return;
  }
  hps::FileSystem fs("/tmp");
  for (auto _ : state) {
    auto chunks = fs.split_file(path, 65536);
    benchmark::DoNotOptimize(chunks);
  }
  std::remove(path.c_str());
}

BENCHMARK(BM_FileSystem_SplitFile)->Arg(1048576)->Arg(10485760);

static void BM_FileSystem_StoreRead(benchmark::State& state) {
  std::size_t size = static_cast<std::size_t>(state.range(0));
  std::vector<char> data(size, 'x');
  hps::FileSystem fs("/tmp");
  std::string vpath = "bench_test_file";
  for (auto _ : state) {
    fs.store_file(vpath, data);
    auto read_back = fs.read_file(vpath);
    benchmark::DoNotOptimize(read_back);
  }
  fs.delete_file(vpath);
  state.SetBytesProcessed(state.iterations() * size * 2);
}

BENCHMARK(BM_FileSystem_StoreRead)->Arg(1024)->Arg(65536)->Arg(1048576);

static void BM_FileSystem_ConcurrentStoreRead(benchmark::State& state) {
  std::size_t size = static_cast<std::size_t>(state.range(0));
  int thread_count = state.range(1);
  std::vector<char> data(size, 'y');
  hps::FileSystem fs("/tmp");
  for (auto _ : state) {
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(thread_count));
    for (int t = 0; t < thread_count; ++t) {
      threads.emplace_back([&, t] {
        std::string vpath = "bench_concurrent_" + std::to_string(t);
        fs.store_file(vpath, data);
        auto read_back = fs.read_file(vpath);
        fs.delete_file(vpath);
        benchmark::DoNotOptimize(read_back);
      });
    }
    for (auto& t : threads) t.join();
  }
  state.SetBytesProcessed(state.iterations() * size * 2 * thread_count);
}

BENCHMARK(BM_FileSystem_ConcurrentStoreRead)->Args({1024, 4})->Args({65536, 4});

static void BM_FileSystem_ResolvePath(benchmark::State& state) {
  hps::FileSystem fs("/tmp");
  std::vector<std::string> vpaths = {
    "file.txt",
    "a/b/c/d/file.txt",
    "a/b/c/d/e/f/g/h/i/j/k/l/m/file.txt",
  };
  std::vector<char> data(16, 'x');
  for (auto _ : state) {
    for (auto& vp : vpaths) {
      fs.store_file(vp, data);
      auto read_back = fs.read_file(vp);
      fs.delete_file(vp);
      benchmark::DoNotOptimize(read_back);
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(vpaths.size()));
}

BENCHMARK(BM_FileSystem_ResolvePath);

BENCHMARK_MAIN();
