#pragma once

#include "connection.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace hps {

/**
 * TCP 服务器接口（抽象类）
 *
 * 基于 epoll ET + ThreadPool 的高性能 TCP 服务器抽象。
 * 连接创建/释放非高频路径，使用动态多态。
 */
class ITcpServer {
public:
  using Handler = std::function<void(std::shared_ptr<Connection>)>;

  virtual ~ITcpServer() = default;

  virtual bool init() = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void set_handler(Handler handler) = 0;
  virtual uint16_t actual_port() const = 0;
};

}  // namespace hps
