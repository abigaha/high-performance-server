#include "http_parser.h"

#include <cstdlib>

namespace hps {

HttpParser::HttpParser() {
  reset();
}

void HttpParser::reset() {
  state_ = ParserState::REQUEST_LINE;
  error_ = ParserError::OK;
  request_.clear();
  reset_line_buf();
  body_bytes_remaining_ = 0;
  chunk_size_ = 0;
  chunk_bytes_read_ = 0;
  chunk_need_crlf_ = false;
  total_body_bytes_ = 0;
  streaming_mode_ = false;
  chunk_handler_ = nullptr;
  body_done_cb_ = nullptr;
  headers_done_cb_ = nullptr;
  chunk_buf_.clear();
  stream_chunk_size_ = static_cast<std::size_t>(kStreamChunkSize);
}

void HttpParser::reset_line_buf() {
  line_buf_.clear();
}

ParserResult HttpParser::feed(std::string_view data) {
  if (state_ == ParserState::COMPLETE || error_ != ParserError::OK) {
    return {.err = error_, .consumed = 0};
  }
  size_t i = 0;
  for (; i < data.size() && error_ == ParserError::OK; ++i) {
    const char c = data[i];
    switch (state_) {
      case ParserState::REQUEST_LINE:
        feed_request_line(c);
        break;
      case ParserState::HEADERS:
        feed_headers(c);
        break;
      case ParserState::BODY_IDENTITY:
        feed_body_identity(c);
        break;
      case ParserState::BODY_CHUNK_SIZE:
        feed_chunk_size(c);
        break;
      case ParserState::BODY_CHUNK_DATA:
        feed_chunk_data(c);
        break;
      case ParserState::BODY_CHUNK_TRAILER:
        feed_chunk_trailer(c);
        break;
      default:
        break;
    }
  }
  if (error_ != ParserError::OK) {
    return {.err = error_, .consumed = i};
  }
  if (state_ == ParserState::COMPLETE) {
    return {.err = ParserError::OK, .consumed = i};
  }
  return {.err = ParserError::INCOMPLETE, .consumed = i};
}

// ==================== 请求行解析 ====================

ParserResult HttpParser::feed_request_line(char c) {
  if (c == '\n') {
    if (!line_buf_.empty() && line_buf_.back() == '\r') {
      line_buf_.pop_back();
    }
    auto sp1 = line_buf_.find(' ');
    if (sp1 == std::string::npos) {
      error_ = ParserError::BAD_REQUEST;
      return {.err = error_, .consumed = 1};
    }
    request_.method = string_to_http_method(std::string_view(line_buf_.data(), sp1));

    auto sp2 = line_buf_.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
      error_ = ParserError::BAD_REQUEST;
      return {.err = error_, .consumed = 1};
    }
    std::string_view line_sv(line_buf_);
    std::string_view uri = line_sv.substr(sp1 + 1, sp2 - sp1 - 1);
    auto qpos = uri.find('?');
    if (qpos != std::string_view::npos) {
      request_.path = uri.substr(0, qpos);
      request_.query_string = uri.substr(qpos + 1);
    } else {
      request_.path = uri;
    }

    request_.version = line_buf_.substr(sp2 + 1);

    state_ = ParserState::HEADERS;
    reset_line_buf();
  } else {
    line_buf_.push_back(c);
  }
  return {.err = ParserError::OK, .consumed = 1};
}

// ==================== 请求头解析 ====================

void HttpParser::handle_end_of_headers() {
  if (headers_done_cb_) {
    headers_done_cb_(request_);
    headers_done_cb_ = nullptr;
  }
  auto it = request_.headers.find("Content-Length");
  if (it != request_.headers.end()) {
    uint64_t cl = 0;
    try {
      cl = std::stoull(it->second);
    } catch (...) {
      error_ = ParserError::BAD_REQUEST;
      return;
    }
    if (cl > kMaxBodySize) {
      error_ = ParserError::PAYLOAD_TOO_LARGE;
      return;
    }
    body_bytes_remaining_ = cl;
    total_body_bytes_ = 0;
    if (cl == 0) {
      finish_body();
      if (error_ != ParserError::OK) {
        return;
      }
      state_ = ParserState::COMPLETE;
    } else {
      state_ = ParserState::BODY_IDENTITY;
    }
    return;
  }
  auto it_te = request_.headers.find("Transfer-Encoding");
  if (it_te != request_.headers.end() && it_te->second.find("chunked") != std::string::npos) {
    state_ = ParserState::BODY_CHUNK_SIZE;
    reset_line_buf();
    return;
  }
  state_ = ParserState::COMPLETE;
}

void HttpParser::parse_header_line() {
  auto colon = line_buf_.find(':');
  if (colon == std::string::npos) {
    error_ = ParserError::BAD_REQUEST;
    return;
  }
  std::string key(line_buf_.data(), colon);
  std::string value;
  if (colon + 1 < line_buf_.size()) {
    auto val_start = colon + 1;
    while (val_start < line_buf_.size() && line_buf_[val_start] == ' ') {
      ++val_start;
    }
    value = line_buf_.substr(val_start);
  }
  request_.headers[std::move(key)] = std::move(value);
}

ParserResult HttpParser::feed_headers(char c) {
  if (c == '\n') {
    if (!line_buf_.empty() && line_buf_.back() == '\r') {
      line_buf_.pop_back();
    }
    if (line_buf_.empty()) {
      handle_end_of_headers();
      reset_line_buf();
      return {.err = error_ != ParserError::OK ? error_ : ParserError::OK, .consumed = 1};
    }
    parse_header_line();
    reset_line_buf();
    if (error_ != ParserError::OK) {
      return {.err = error_, .consumed = 1};
    }
  } else {
    line_buf_.push_back(c);
  }
  return {.err = ParserError::OK, .consumed = 1};
}

// ==================== Body（Content-Length）解析 ====================

void HttpParser::flush_chunk_buf() {
  if (chunk_buf_.empty()) {
    return;
  }
  if (chunk_handler_ && !chunk_handler_(chunk_buf_)) {
    error_ = ParserError::BAD_REQUEST;
  }
  chunk_buf_.clear();
}

void HttpParser::finish_body() {
  if (streaming_mode_) {
    flush_chunk_buf();
  }
  if (error_ != ParserError::OK || !body_done_cb_) {
    return;
  }
  auto callback = std::move(body_done_cb_);
  if (!callback()) {
    error_ = ParserError::BAD_REQUEST;
  }
}

void HttpParser::append_body_byte(char c) {
  if (!streaming_mode_) {
    request_.body.push_back(c);
    return;
  }
  chunk_buf_.push_back(c);
  if (chunk_buf_.size() >= stream_chunk_size_) {
    flush_chunk_buf();
  }
}

ParserResult HttpParser::feed_body_identity(char c) {
  if (body_bytes_remaining_ > 0) {
    append_body_byte(c);
    if (error_ != ParserError::OK) {
      return {.err = error_, .consumed = 1};
    }
    --body_bytes_remaining_;
    ++total_body_bytes_;
    if (total_body_bytes_ > kMaxBodySize) {
      error_ = ParserError::PAYLOAD_TOO_LARGE;
      return {.err = error_, .consumed = 1};
    }
  }
  if (body_bytes_remaining_ == 0) {
    finish_body();
    if (error_ != ParserError::OK) {
      return {.err = error_, .consumed = 1};
    }
    state_ = ParserState::COMPLETE;
  }
  return {.err = ParserError::OK, .consumed = 1};
}

// ==================== Chunked 编码解析 ====================

ParserResult HttpParser::feed_chunk_size(char c) {
  if (c == '\n') {
    if (!line_buf_.empty() && line_buf_.back() == '\r') {
      line_buf_.pop_back();
    }
    auto semi = line_buf_.find(';');
    std::string hex_str = (semi == std::string::npos) ? line_buf_ : line_buf_.substr(0, semi);
    char* end = nullptr;
    // N10-L：strtoul 返回 unsigned long，32 位系统截断；改 strtoull 返回 unsigned long long
    chunk_size_ = std::strtoull(hex_str.c_str(), &end, 16);
    if (end == hex_str.c_str() || *end != '\0') {
      error_ = ParserError::BAD_REQUEST;
      return {.err = error_, .consumed = 1};
    }
    chunk_bytes_read_ = 0;
    if (chunk_size_ == 0) {
      finish_body();
      if (error_ != ParserError::OK) {
        return {.err = error_, .consumed = 1};
      }
      state_ = ParserState::BODY_CHUNK_TRAILER;
    } else {
      if (total_body_bytes_ + chunk_size_ > kMaxBodySize) {
        error_ = ParserError::PAYLOAD_TOO_LARGE;
        return {.err = error_, .consumed = 1};
      }
      state_ = ParserState::BODY_CHUNK_DATA;
    }
    reset_line_buf();
  } else {
    line_buf_.push_back(c);
  }
  return {.err = ParserError::OK, .consumed = 1};
}

ParserResult HttpParser::feed_chunk_data(char c) {
  (void)c;
  if (!chunk_need_crlf_) {
    append_body_byte(c);
    if (error_ != ParserError::OK) {
      return {.err = error_, .consumed = 1};
    }
    ++chunk_bytes_read_;
    ++total_body_bytes_;
    if (total_body_bytes_ > kMaxBodySize) {
      error_ = ParserError::PAYLOAD_TOO_LARGE;
      return {.err = error_, .consumed = 1};
    }
    if (chunk_bytes_read_ == chunk_size_) {
      chunk_need_crlf_ = true;
    }
  } else {
    if (c == '\n') {
      chunk_need_crlf_ = false;
      state_ = ParserState::BODY_CHUNK_SIZE;
      reset_line_buf();
    }
  }
  return {.err = ParserError::OK, .consumed = 1};
}

ParserResult HttpParser::feed_chunk_trailer(char c) {
  if (c == '\n') {
    if (!line_buf_.empty() && line_buf_.back() == '\r') {
      line_buf_.pop_back();
    }
    if (line_buf_.empty()) {
      state_ = ParserState::COMPLETE;
    } else {
      reset_line_buf();
    }
  } else {
    line_buf_.push_back(c);
  }
  return {.err = ParserError::OK, .consumed = 1};
}

} // namespace hps
