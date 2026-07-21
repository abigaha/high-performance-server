#include "http_response.h"
#include "qps_runner.hpp"
#include "range_parser.h"

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kSmallBody = R"({"status":"ok","items":3})";
constexpr std::size_t kLargeBodySize = 64 * 1024;
constexpr std::size_t kResourceSize = 1024 * 1024;

bool is_cleared(const hps::HttpResponse& response) {
  return response.status_code == 200 && response.status_text == "OK" && response.headers.empty() &&
         response.body.empty();
}

int run_benchmark() {
  const auto levels = hps::bench::default_qps_levels();

  hps::bench::run_qps_steps("HttpResponse small body serialize+clear", levels, [](int) {
    thread_local hps::HttpResponse response;
    response.set_content_type("application/json");
    response.body = kSmallBody;
    response.set_content_length(response.body.size());

    const std::string serialized = response.serialize();
    const bool valid = serialized.starts_with("HTTP/1.1 200 OK\r\n") &&
                       serialized.find("Content-Type: application/json\r\n") != std::string::npos &&
                       serialized.ends_with(kSmallBody);
    response.clear();
    return valid && is_cleared(response);
  });

  const std::string large_body(kLargeBodySize, 'x');
  hps::bench::run_qps_steps("HttpResponse large body 64KB serialize+clear", levels, [&large_body](int) {
    thread_local hps::HttpResponse response;
    response.set_content_type("application/octet-stream");
    response.body = large_body;
    response.set_content_length(response.body.size());

    const std::string serialized = response.serialize();
    const bool valid = serialized.size() > large_body.size() && serialized.ends_with(large_body);
    response.clear();
    return valid && is_cleared(response);
  });

  static constexpr std::array<std::pair<std::string_view, std::string_view>, 12> kHeaders = {{
    {"Cache-Control", "no-cache"},
    {"Content-Language", "zh-CN"},
    {"ETag", "qps-response-etag"},
    {"Last-Modified", "Tue, 21 Jul 2026 08:00:00 GMT"},
    {"Server", "hps-qps"},
    {"Vary", "Accept-Encoding"},
    {"X-Content-Type-Options", "nosniff"},
    {"X-Frame-Options", "DENY"},
    {"X-Request-ID", "qps-request-20260721"},
    {"X-RateLimit-Limit", "10000"},
    {"X-RateLimit-Remaining", "9999"},
    {"X-XSS-Protection", "1; mode=block"},
  }};
  hps::bench::run_qps_steps("HttpResponse 12 headers serialize+clear", levels, [](int) {
    thread_local hps::HttpResponse response;
    for (const auto& [key, value] : kHeaders) {
      response.set_header(key, value);
    }
    response.body = "header-benchmark";
    response.set_content_length(response.body.size());

    const std::string serialized = response.serialize();
    const bool valid = response.headers.size() == kHeaders.size() + 1 &&
                       serialized.find("X-Request-ID: qps-request-20260721\r\n") != std::string::npos &&
                       serialized.ends_with(response.body);
    response.clear();
    return valid && is_cleared(response);
  });

  const hps::RangeRequest partial_range = hps::parse_range_header("bytes=1024-2047", kResourceSize);
  hps::bench::run_qps_steps("HttpResponse 206 serialize+clear", levels, [&partial_range](int) {
    thread_local hps::HttpResponse response;
    hps::build_206_headers(response, partial_range, kResourceSize);
    response.body.assign(1024, 'p');

    const std::string serialized = response.serialize();
    const bool valid = response.status_code == 206 && response.headers["content-range"] == "bytes 1024-2047/1048576" &&
                       serialized.starts_with("HTTP/1.1 206 Partial Content\r\n") &&
                       serialized.ends_with(response.body);
    response.clear();
    return valid && is_cleared(response);
  });

  hps::bench::run_qps_steps("HttpResponse 416 serialize+clear", levels, [](int) {
    thread_local hps::HttpResponse response;
    hps::build_416_response(response, kResourceSize);

    const std::string serialized = response.serialize();
    const bool valid = response.status_code == 416 && response.headers["CONTENT-RANGE"] == "bytes */1048576" &&
                       serialized.starts_with("HTTP/1.1 416 Range Not Satisfiable\r\n") &&
                       serialized.ends_with("416 Range Not Satisfiable");
    response.clear();
    return valid && is_cleared(response);
  });

  return EXIT_SUCCESS;
}

} // namespace

int main() noexcept {
  try {
    return run_benchmark();
  } catch (const std::exception& error) {
    std::cerr << "HTTP 响应 QPS 基准失败: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "HTTP 响应 QPS 基准失败: 未知异常\n";
  }
  return EXIT_FAILURE;
}
