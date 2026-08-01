#pragma once

#include "case_insensitive.h"
#include "models.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace hps {
/** HTTP 请求方法 */
enum class HttpMethod {
  UNKNOWN = 0, ///< 未知/无效方法
  GET,         ///< GET
  HEAD,        ///< HEAD
  POST,        ///< POST
  PUT,         ///< PUT
  DELETE,      ///< DELETE
  CONNECT,     ///< CONNECT
  OPTIONS,     ///< OPTIONS
  TRACE,       ///< TRACE
  PATCH,       ///< PATCH
};

/** HttpMethod → 字符串（如 "GET"） */
std::string_view http_method_to_string(HttpMethod m) noexcept;

/** 字符串 → HttpMethod（如 "GET" → HttpMethod::GET） */
HttpMethod string_to_http_method(std::string_view s) noexcept;

/** HTTP 请求结构体 */
struct HttpRequest {
  HttpMethod method = HttpMethod::UNKNOWN;                           ///< 请求方法
  std::string path;                                                  ///< 请求路径（不含查询参数）
  std::string query_string;                                          ///< 查询参数字符串（? 之后的部分）
  std::string version = "HTTP/1.1";                                  ///< HTTP 版本
  HeaderMap headers;                                                 ///< 请求头（大小写不敏感）
  std::string body;                                                  ///< 请求体
  std::unordered_map<std::string, std::string> path_params;          ///< 路由参数（如 {"id":"42"}）
  TokenValidationStatus auth_status{TokenValidationStatus::INVALID}; ///< Token 验证状态
  EffectiveIdentity auth_user;                                       ///< 当前有效认证身份

  /** 重置为默认状态 */
  void clear() noexcept;
};

} // namespace hps
