#pragma once

#include "i_tcp_client.h"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>

namespace hps {

class TcpClient : public ITcpClient {
public:
  TcpClient(const std::string& server_ip, uint16_t server_port);
  /** @param connect_timeout_ms 连接超时（毫秒），仅在 RAW 模式下受支持；LINE 模式使用默认超时 */
  TcpClient(uint32_t connect_timeout_ms, const std::string& server_ip, uint16_t server_port);
  ~TcpClient() override;
  TcpClient(const TcpClient&) = delete;
  TcpClient& operator=(const TcpClient&) = delete;
  TcpClient(TcpClient&& other) noexcept;
  TcpClient& operator=(TcpClient&& other) noexcept;

  bool connect_to_server() override;
  void disconnect() override;

  bool send_message(const std::string& message) const override;
  // NOLINTNEXTLINE(google-default-arguments): 调用方借基类默认值，override 需保持一致
  bool receive_message(std::string& message, ReadMode mode = ReadMode::LINE, uint32_t read_timeout_ms = 5000) override;

  bool send_file(const std::string& file_path) const;
  bool send_file(const std::string& data, std::size_t size) const;

  bool is_connected() const override { return client_sockfd_ >= 0; }

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
