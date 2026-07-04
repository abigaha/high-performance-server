#pragma once

#include "i_router.h"

#include <cstdint>
#include <string_view>

namespace hps {

/**
 * HTTP 服务器接口（抽象类）
 *
 * 封装 TCP 服务器 + 路由器，提供 HTTP 请求路由分发。
 * 非热点路径，使用动态多态。
 */
class IHttpServer {
public:
  using Handler = IRouter::Handler;

  virtual ~IHttpServer() = default;

  virtual bool init() = 0;
  virtual void start() = 0;
  virtual void stop() = 0;

  virtual void get(std::string_view path, Handler handler) = 0;
  virtual void post(std::string_view path, Handler handler) = 0;
  virtual void put(std::string_view path, Handler handler) = 0;
  virtual void del(std::string_view path, Handler handler) = 0;

  virtual uint16_t actual_port() const = 0;
};

} // namespace hps
