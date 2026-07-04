#include "chunk_header.h"

#include <arpa/inet.h>

#include <cstdint>

namespace hps {

void ChunkHeader::to_network() {
  magic = htonl(magic);
  chunk_index = htonl(chunk_index);
  offset = htobe64(offset);
  chunk_size = htobe64(chunk_size);
  total_chunks = htonl(total_chunks);
}

void ChunkHeader::from_network() {
  magic = ntohl(magic);
  chunk_index = ntohl(chunk_index);
  offset = be64toh(offset);
  chunk_size = be64toh(chunk_size);
  total_chunks = ntohl(total_chunks);
}

} // namespace hps
