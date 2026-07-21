#include "http_request.h"
#include "qps_runner.hpp"
#include "range_parser.h"
#include "url_decode.h"

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

int run_benchmark() {
  const auto levels = hps::bench::default_qps_levels();

  const std::string encoded_path = "/api/search%20results%2F2026?q=high+performance%20server";
  const std::string decoded_path = "/api/search results/2026?q=high performance server";
  hps::bench::run_qps_steps("HTTP utility url_decode valid", levels, [&](int) {
    thread_local std::string output;
    return hps::url_decode(encoded_path, output) && output == decoded_path;
  });

  const std::string invalid_path = "/api/items/%GG";
  hps::bench::run_qps_steps("HTTP utility url_decode rejected input", levels, [&](int) {
    thread_local std::string output;
    return !hps::url_decode(invalid_path, output);
  });

  hps::bench::run_qps_steps("HTTP utility range parser multi-range", levels, [](int) {
    const hps::RangeRequest range = hps::parse_range_header("bytes=0-99, 4096-8191, -512", 1024 * 1024);
    return range.valid && range.satisfiable && range.ranges.size() == 3 && range.ranges[0].start == 0 &&
           range.ranges[0].end == 100 && range.ranges[1].start == 4096 && range.ranges[1].end == 8192 &&
           range.ranges[2].start == 1024 * 1024 - 512 && range.ranges[2].end == 1024 * 1024;
  });

  hps::bench::run_qps_steps("HTTP utility range parser unsatisfiable", levels, [](int) {
    const hps::RangeRequest range = hps::parse_range_header("bytes=2097152-3145727", 1024 * 1024);
    return range.valid && !range.satisfiable && range.ranges.size() == 1 && range.ranges[0].start == 1024 * 1024 &&
           range.ranges[0].end == 1024 * 1024;
  });

  static constexpr std::array<std::pair<hps::HttpMethod, std::string_view>, 9> kMethods = {{
    {hps::HttpMethod::GET, "GET"},
    {hps::HttpMethod::HEAD, "HEAD"},
    {hps::HttpMethod::POST, "POST"},
    {hps::HttpMethod::PUT, "PUT"},
    {hps::HttpMethod::DELETE, "DELETE"},
    {hps::HttpMethod::CONNECT, "CONNECT"},
    {hps::HttpMethod::OPTIONS, "OPTIONS"},
    {hps::HttpMethod::TRACE, "TRACE"},
    {hps::HttpMethod::PATCH, "PATCH"},
  }};
  hps::bench::run_qps_steps("HTTP utility method conversion roundtrip", levels, [](int) {
    for (const auto& [method, text] : kMethods) {
      if (hps::http_method_to_string(method) != text || hps::string_to_http_method(text) != method) {
        return false;
      }
    }
    return hps::string_to_http_method("INVALID") == hps::HttpMethod::UNKNOWN;
  });

  hps::bench::run_qps_steps("HTTP utility case-insensitive headers", levels, [](int) {
    thread_local hps::HttpRequest request;
    request.headers["Content-Type"] = "application/json";
    request.headers["X-Request-ID"] = "qps-20260721";
    request.headers["ACCEPT-ENCODING"] = "gzip, br";

    const bool valid = request.headers["content-type"] == "application/json" &&
                       request.headers["x-request-id"] == "qps-20260721" &&
                       request.headers["Accept-Encoding"] == "gzip, br" && request.headers.size() == 3;
    request.clear();
    return valid && request.headers.empty();
  });

  return EXIT_SUCCESS;
}

} // namespace

int main() noexcept {
  try {
    return run_benchmark();
  } catch (const std::exception& error) {
    std::cerr << "HTTP 工具 QPS 基准失败: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "HTTP 工具 QPS 基准失败: 未知异常\n";
  }
  return EXIT_FAILURE;
}
