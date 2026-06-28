#pragma once

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>

namespace hps {

enum class ReadMode { Line, Raw };

class CTcpClient {
public:
  CTcpClient(const std::string& server_ip, uint16_t server_port);
  CTcpClient(uint32_t connect_timeout_ms, const std::string& server_ip, uint16_t server_port);
  ~CTcpClient();
  CTcpClient(const CTcpClient&) = delete;
  CTcpClient& operator=(const CTcpClient&) = delete;
  CTcpClient(CTcpClient&& other) noexcept;
  CTcpClient& operator=(CTcpClient&& other) noexcept;

  bool connectToServer();
  void disconnect();

  bool sendMessage(const std::string& message) const;
  bool receiveMessage(std::string& message, ReadMode mode = ReadMode::Line, uint32_t read_timeout_ms = 5000);

  bool sendFile(const std::string& file_path) const;
  bool sendFile(const std::string& data, std::size_t size) const;

  bool is_connected() const { return client_sockfd_ >= 0; }

private:
  static bool send_all(int fd, const char* data, std::size_t size);

  struct DurationMs {
    uint32_t value;
  };

  bool wait_connected(int fd, DurationMs timeout);
  bool wait_readable(int fd, DurationMs timeout);
  bool recv_into_buffer(DurationMs timeout);
  bool read_line(std::string& line);
  bool read_raw(std::string& data, uint32_t timeout_ms);

  int client_sockfd_{-1};
  std::string server_ip_;
  uint16_t server_port_;
  uint32_t connect_timeout_ms_;
  std::string read_buf_;
};

} // namespace hps
