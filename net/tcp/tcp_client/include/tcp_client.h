#pragma once

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>

namespace hps {

enum class ReadMode { LINE, RAW };

class TcpClient {
public:
  TcpClient(const std::string& server_ip, uint16_t server_port);
  /** @param connect_timeout_ms 连接超时（毫秒），仅在 Raw 模式下受支持；Line 模式使用默认超时 */
  TcpClient(uint32_t connect_timeout_ms, const std::string& server_ip, uint16_t server_port);
  ~TcpClient();
  TcpClient(const TcpClient&) = delete;
  TcpClient& operator=(const TcpClient&) = delete;
  TcpClient(TcpClient&& other) noexcept;
  TcpClient& operator=(TcpClient&& other) noexcept;

  bool connect_to_server();
  void disconnect();

  bool send_message(const std::string& message) const;
  bool receive_message(std::string& message, ReadMode mode = ReadMode::LINE, uint32_t read_timeout_ms = 5000);

  bool send_file(const std::string& file_path) const;
  bool send_file(const std::string& data, std::size_t size) const;

  bool is_connected() const { return client_sockfd_ >= 0; }

private:
  struct DurationMs {
    uint32_t value;
  };

  static bool send_all(int fd, const char* data, std::size_t size, DurationMs timeout);

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
  bool peer_closed_{false};
};

} // namespace hps
