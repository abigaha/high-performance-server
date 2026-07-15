#include "chunk_header.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <cstring>

static void BM_ChunkHeader_ToNetwork(benchmark::State& state) {
  hps::ChunkHeader h;
  h.magic = hps::kChunkMagic;
  h.chunk_index = 12345;
  h.offset = 9876543210ULL;
  h.chunk_size = 65536;
  h.total_chunks = 100;
  for (auto _ : state) {
    h.to_network();
    benchmark::DoNotOptimize(h);
    h.from_network();
  }
}

BENCHMARK(BM_ChunkHeader_ToNetwork);

static void BM_ChunkHeader_FromNetwork(benchmark::State& state) {
  hps::ChunkHeader h;
  h.magic = hps::kChunkMagic;
  h.chunk_index = 12345;
  h.offset = 9876543210ULL;
  h.chunk_size = 65536;
  h.total_chunks = 100;
  h.to_network();
  for (auto _ : state) {
    h.from_network();
    benchmark::DoNotOptimize(h);
    h.to_network();
  }
}

BENCHMARK(BM_ChunkHeader_FromNetwork);

static void BM_ChunkHeader_Serialize(benchmark::State& state) {
  hps::ChunkHeader h;
  h.magic = hps::kChunkMagic;
  h.chunk_index = 12345;
  h.offset = 9876543210ULL;
  h.chunk_size = 65536;
  h.total_chunks = 100;
  h.to_network();
  std::array<char, hps::kChunkHeaderSize> buf{};
  for (auto _ : state) {
    std::memcpy(buf.data(), &h.magic, 4);
    std::memcpy(buf.data() + 4, &h.chunk_index, 4);
    std::memcpy(buf.data() + 8, &h.offset, 8);
    std::memcpy(buf.data() + 16, &h.chunk_size, 8);
    std::memcpy(buf.data() + 24, &h.total_chunks, 4);
    benchmark::DoNotOptimize(buf);
  }
}

BENCHMARK(BM_ChunkHeader_Serialize);

BENCHMARK_MAIN();
