#include "chunk_header.h"
#include "qps_runner.hpp"

#include <array>
#include <cstring>

int main() {
  auto levels = hps::bench::default_qps_levels();

  // ToNetwork + FromNetwork cycle
  {
    hps::ChunkHeader h;
    h.magic = hps::kChunkMagic;
    h.chunk_index = 12345;
    h.offset = 9876543210ULL;
    h.chunk_size = 65536;
    h.total_chunks = 100;

    hps::bench::run_qps_steps("ChunkHeader To/From Network", levels, [&h](int) {
      h.to_network();
      h.from_network();
    });
  }

  // Serialize with memcpy
  {
    hps::ChunkHeader h;
    h.magic = hps::kChunkMagic;
    h.chunk_index = 12345;
    h.offset = 9876543210ULL;
    h.chunk_size = 65536;
    h.total_chunks = 100;
    h.to_network();
    std::array<char, hps::kChunkHeaderSize> buf{};

    hps::bench::run_qps_steps("ChunkHeader Serialize memcpy", levels, [&h, &buf](int) {
      std::memcpy(buf.data(), &h.magic, 4);
      std::memcpy(buf.data() + 4, &h.chunk_index, 4);
      std::memcpy(buf.data() + 8, &h.offset, 8);
      std::memcpy(buf.data() + 16, &h.chunk_size, 8);
      std::memcpy(buf.data() + 24, &h.total_chunks, 4);
      (void)buf;
    });
  }

  return 0;
}
