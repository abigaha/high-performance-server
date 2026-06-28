#include "http_response.h"

#include <sstream>

namespace hps {

void HttpResponse::set_status(int code, std::string_view text) noexcept {
  status_code = code;
  status_text = text;
}

void HttpResponse::set_header(std::string_view key, std::string_view value) {
  headers[std::string(key)] = std::string(value);
}

void HttpResponse::set_content_type(std::string_view type) {
  set_header("Content-Type", type);
}

void HttpResponse::set_content_length(uint64_t len) {
  headers["Content-Length"] = std::to_string(len);
}

std::string HttpResponse::serialize() const {
  std::ostringstream oss;
  oss << version << ' ' << status_code << ' ' << status_text << "\r\n";
  for (const auto& [key, value] : headers) {
    oss << key << ": " << value << "\r\n";
  }
  oss << "\r\n";
  oss << body;
  return oss.str();
}

void HttpResponse::clear() noexcept {
  status_code = 200;
  status_text = "OK";
  headers.clear();
  body.clear();
}

} // namespace hps
