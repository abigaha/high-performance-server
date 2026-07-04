#pragma once

#include <cstdint>
#include <string>

namespace hps {

class ITcpClient;
class ITcpServer;

class IFileTransfer {
public:
  virtual ~IFileTransfer() = default;

  virtual bool transfer_small(const std::string& path, ITcpClient& client) = 0;

  virtual bool transfer_large(const std::string& path, const std::string& peer_ip, uint16_t peer_port) = 0;

  virtual bool receive_file(const std::string& save_path, ITcpServer& server) = 0;
};

} // namespace hps
