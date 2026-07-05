#pragma once

#include "http_request.h"
#include "http_response.h"

#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>

namespace hps {

/**
 * HTTP 路由器接口（抽象类）
 *
 * 基于前缀树（trie）的路由匹配，支持静态段与参数段（:name）。
 * 非热点路径，使用动态多态。
 */
class IRouter {
public:
  using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;
  using Params = std::unordered_map<std::string, std::string>;

  virtual ~IRouter() = default;

  virtual void add(HttpMethod method, std::string_view path, Handler handler) = 0;
  virtual bool match(HttpMethod method, std::string_view path, Handler& outHandler, Params& outParams) const = 0;
  virtual bool path_exists(std::string_view path) const = 0;
};

} // namespace hps
