#pragma once

#include "http_request.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace hps {

/** HTTP 解析器状态 */
enum class ParserState {
  REQUEST_LINE,
  HEADERS,
  BODY_IDENTITY,
  BODY_CHUNK_SIZE,
  BODY_CHUNK_DATA,
  BODY_CHUNK_TRAILER,
  COMPLETE,
};

/** 解析错误类型 */
enum class ParserError {
  OK = 0,
  INCOMPLETE,
  BAD_REQUEST,
  PAYLOAD_TOO_LARGE,
};

/** 解析结果 */
struct ParserResult {
  ParserError err = ParserError::OK;
  size_t consumed = 0;
};

/**
 * HTTP 请求流式解析器
 *
 * 支持流式 body 处理：当设置 streaming_mode 时，body 数据不会累积到
 * request_.body，而是以 2MB 分片为单位通过 chunk_handler_ 回调传递。
 */
class HttpParser {
public:
  static constexpr uint64_t kMaxBodySize = static_cast<uint64_t>(100) * 1024 * 1024;
  static constexpr uint64_t kStreamChunkSize = 2097152;

  using ChunkHandler = std::function<bool(std::string_view chunk)>;
  using HeadersDoneCallback = std::function<void(const HttpRequest&)>;
  using BodyDoneCallback = std::function<bool()>;

  HttpParser();

  ParserResult feed(std::string_view data);

  ParserState state() const noexcept { return state_; }

  ParserError error() const noexcept { return error_; }

  const HttpRequest& request() const noexcept { return request_; }

  HttpRequest& request() noexcept { return request_; }

  void set_streaming_mode(bool enable) { streaming_mode_ = enable; }

  bool streaming_mode() const { return streaming_mode_; }

  void set_chunk_handler(ChunkHandler cb) { chunk_handler_ = std::move(cb); }

  void set_stream_chunk_size(std::size_t size) noexcept {
    stream_chunk_size_ = size == 0 ? static_cast<std::size_t>(kStreamChunkSize) : size;
  }

  void set_body_done_callback(BodyDoneCallback cb) { body_done_cb_ = std::move(cb); }

  void set_headers_done_callback(HeadersDoneCallback cb) { headers_done_cb_ = std::move(cb); }

  void reset();

private:
  void reset_line_buf();
  void flush_chunk_buf();
  void finish_body();
  void append_body_byte(char c);
  ParserResult feed_request_line(char c);
  ParserResult feed_headers(char c);
  void handle_end_of_headers();
  void parse_header_line();
  ParserResult feed_body_identity(char c);
  ParserResult feed_chunk_size(char c);
  ParserResult feed_chunk_data(char c);
  ParserResult feed_chunk_trailer(char c);

  ParserState state_{ParserState::REQUEST_LINE};
  ParserError error_{ParserError::OK};
  HttpRequest request_;
  std::string line_buf_;
  uint64_t body_bytes_remaining_{0};
  uint64_t chunk_size_{0};
  uint64_t chunk_bytes_read_{0};
  bool chunk_need_crlf_{false};
  uint64_t total_body_bytes_{0};

  bool streaming_mode_{false};
  ChunkHandler chunk_handler_;
  BodyDoneCallback body_done_cb_;
  HeadersDoneCallback headers_done_cb_;
  std::string chunk_buf_;
  std::size_t stream_chunk_size_{static_cast<std::size_t>(kStreamChunkSize)};
};

} // namespace hps
