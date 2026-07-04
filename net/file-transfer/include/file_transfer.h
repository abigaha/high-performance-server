#pragma once

#include "i_file_transfer.h"

#include <cstddef>
#include <memory>

namespace hps {

class IFileSystem;

class FileTransfer : public IFileTransfer {
public:
  explicit FileTransfer(std::shared_ptr<IFileSystem> fs, std::size_t chunk_size = kDefaultChunkSize);

  bool transfer_small(const std::string& path, ITcpClient& client) override;

  bool transfer_large(const std::string& path, const std::string& peer_ip, uint16_t peer_port) override;

  bool receive_file(const std::string& save_path, ITcpServer& server) override;

private:
  static constexpr std::size_t kDefaultChunkSize = 2 * 1024 * 1024;
  static constexpr int kMaxConcurrency = 16;

  bool recv_all(int fd, void* buf, std::size_t size);

  std::shared_ptr<IFileSystem> fs_;
  std::size_t chunk_size_;
};

} // namespace hps
