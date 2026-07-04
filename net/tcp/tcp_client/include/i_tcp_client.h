#pragma once

#include <cstdint>
#include <string>

namespace hps {

enum class ReadMode { LINE, RAW };

/**
 * TCP 客户端接口（抽象类）
 *
 * 非阻塞 TCP 客户端接口。连接创建/释放非高频路径，使用动态多态。
 */
class ITcpClient {
public:
  virtual ~ITcpClient() = default;

  virtual bool connect_to_server() = 0;
  virtual void disconnect() = 0;
  virtual bool send_message(const std::string& msg) const = 0;
  // NOLINTNEXTLINE(google-default-arguments): 为保持调用点简洁，基类提供默认参数
  virtual bool receive_message(std::string& msg, ReadMode mode = ReadMode::LINE, uint32_t timeout_ms = 5000) = 0;
  virtual bool is_connected() const = 0;
};

} // namespace hps
