#pragma once

#include "case_insensitive.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace hps {

/** HTTP 响应结构体 */
struct HttpResponse {
  std::string version = "HTTP/1.1"; ///< HTTP 版本
  int status_code = 200;            ///< 状态码（如 200、404、500）
  std::string status_text = "OK";   ///< 状态描述（如 "OK"、"Not Found"）
  HeaderMap headers;                ///< 响应头（大小写不敏感）
  std::string body;                 ///< 响应体

  /**
   * 设置状态码和状态描述
   * @param code 状态码
   * @param text 状态描述
   */
  void set_status(int code, std::string_view text) noexcept;

  /** 设置响应头 */
  void set_header(std::string_view key, std::string_view value);

  /** 快捷设置 Content-Type */
  void set_content_type(std::string_view type);

  /** 快捷设置 Content-Length */
  void set_content_length(uint64_t len);

  /**
   * 序列化为 HTTP 响应字符串
   * @return 完整 HTTP 响应（含状态行、头、空行、body）
   */
  std::string serialize() const;

  /** 重置为默认状态 */
  void clear() noexcept;
};

} // namespace hps
