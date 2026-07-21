#include "chunk_header.h"
#include "qps_runner.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>

namespace {

int run_benchmark() {
  auto levels = hps::bench::default_qps_levels();

  // ToNetwork + FromNetwork cycle
  {
    const hps::ChunkHeader source{
      .magic = hps::kChunkMagic,
      .chunk_index = 12345,
      .offset = 9876543210ULL,
      .chunk_size = 65536,
      .total_chunks = 100,
    };

    hps::bench::run_qps_steps("ChunkHeader To/From Network", levels, [&source](int) {
      auto header = source;
      header.to_network();
      header.from_network();
      return header.magic == source.magic && header.chunk_index == source.chunk_index &&
             header.offset == source.offset && header.chunk_size == source.chunk_size &&
             header.total_chunks == source.total_chunks;
    });
  }

  // Serialize with memcpy
  {
    hps::ChunkHeader network_header{
      .magic = hps::kChunkMagic,
      .chunk_index = 12345,
      .offset = 9876543210ULL,
      .chunk_size = 65536,
      .total_chunks = 100,
    };
    network_header.to_network();

    hps::bench::run_qps_steps("ChunkHeader Serialize memcpy", levels, [&network_header](int) {
      std::array<char, hps::kChunkHeaderSize> buffer{};
      std::memcpy(buffer.data(), &network_header.magic, 4);
      std::memcpy(buffer.data() + 4, &network_header.chunk_index, 4);
      std::memcpy(buffer.data() + 8, &network_header.offset, 8);
      std::memcpy(buffer.data() + 16, &network_header.chunk_size, 8);
      std::memcpy(buffer.data() + 24, &network_header.total_chunks, 4);
      return std::memcmp(buffer.data(), &network_header, hps::kChunkHeaderSize) == 0;
    });
  }

  return 0;
}

} // namespace

int main() noexcept {
  try {
    return run_benchmark();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "分块头 QPS 基准执行失败：%s\n", error.what());
  } catch (...) {
    std::fputs("分块头 QPS 基准执行失败：未知异常\n", stderr);
  }
  return 1;
}
