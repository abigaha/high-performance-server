#pragma once

#include "http_request.h"

#include <cstdint>
#include <string>

namespace hps {

/** HTTP 解析器状态 */
enum class ParserState {
  REQUEST_LINE,       ///< 解析请求行（METHOD PATH VERSION）
  HEADERS,            ///< 解析请求头
  BODY_IDENTITY,      ///< 读取 Content-Length 指定长度的 body
  BODY_CHUNK_SIZE,    ///< 读取 chunk 大小（十六进制）
  BODY_CHUNK_DATA,    ///< 读取 chunk 数据
  BODY_CHUNK_TRAILER, ///< 读取 chunked 尾部（trailer + 最终 CRLF）
  COMPLETE,           ///< 解析完成
};

/** 解析错误类型 */
enum class ParserError {
  OK = 0,            ///< 成功
  INCOMPLETE,        ///< 数据不完整，等待更多数据
  BAD_REQUEST,       ///< 请求格式错误
  PAYLOAD_TOO_LARGE, ///< body 超过最大限制
};

/** 解析结果 */
struct ParserResult {
  ParserError err = ParserError::OK; ///< 错误码
  size_t consumed = 0;               ///< 已消费的字节数
};

/**
 * HTTP 请求流式解析器
 *
 * 状态机设计，支持分片 feed 输入。每次调用 feed() 返回 ParserResult，
 * 包含错误码和已消费的字节数。
 *
 * 最大 body 大小：MAX_BODY_SIZE（100 MiB）
 */
class HttpParser {
public:
  /** body 最大字节数限制（100 MiB） */
  static constexpr uint64_t MAX_BODY_SIZE = static_cast<const uint64_t>(100) * 1024 * 1024;

  HttpParser();

  /**
   * 喂入 HTTP 数据
   * @param data 原始字节数据
   * @return 解析结果（错误码 + 已消费字节数）
   */
  ParserResult feed(std::string_view data);

  /** 当前解析器状态 */
  ParserState state() const noexcept { return state_; }

  /** 当前错误（解析完成后保持） */
  ParserError error() const noexcept { return error_; }

  /** 解析完成的请求（只读） */
  const HttpRequest& request() const noexcept { return request_; }

  /** 解析完成的请求（可写） */
  HttpRequest& request() noexcept { return request_; }

  /** 重置解析器到初始状态 */
  void reset();

private:
  void reset_line_buf();

  // 各状态处理函数（每次处理一个字符）
  /** 处理请求行状态 */
  ParserResult feed_request_line(char c);
  /** 处理请求头状态 */
  ParserResult feed_headers(char c);
  /** 处理头部结束（Content-Length / chunked / 无 body） */
  void handle_end_of_headers();
  /** 解析单行请求头 Key: Value */
  void parse_header_line();
  /** 处理 Content-Length body 状态 */
  ParserResult feed_body_identity(char c);
  /** 处理 chunk size 状态 */
  ParserResult feed_chunk_size(char c);
  /** 处理 chunk data 状态 */
  ParserResult feed_chunk_data(char c);
  /** 处理 chunk trailer 状态 */
  ParserResult feed_chunk_trailer(char c);

  ParserState state_{ParserState::REQUEST_LINE}; ///< 当前解析器状态
  ParserError error_{ParserError::OK};           ///< 当前错误状态
  HttpRequest request_;                          ///< 正在解析的请求
  std::string line_buf_;                         ///< 行缓冲区（累积行数据直到 \n）
  uint64_t body_bytes_remaining_{0};             ///< Identity body 剩余未读字节数
  uint64_t chunk_size_{0};                       ///< 当前 chunk 大小
  uint64_t chunk_bytes_read_{0};                 ///< 当前 chunk 已读字节数
  bool chunk_need_crlf_{false};                  ///< 是否需要跳过 chunk 尾部的 CRLF
  uint64_t total_body_bytes_{0};                 ///< 累计 body 字节数（溢出检测）
};

} // namespace hps
