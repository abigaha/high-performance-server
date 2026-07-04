#pragma once

#include <cstddef>
#include <cstdint>

namespace hps {

constexpr uint32_t kChunkMagic = 0x48505346; // "HPSF"
constexpr std::size_t kChunkHeaderSize = 28;

#pragma pack(push, 1)

struct ChunkHeader {
  uint32_t magic;
  uint32_t chunk_index;
  uint64_t offset;
  uint64_t chunk_size;
  uint32_t total_chunks;

  void to_network();
  void from_network();
};

#pragma pack(pop)

} // namespace hps
