#include "http_parser.h"
#include "qps_runner.hpp"

#include <string>
#include <string_view>

namespace {

std::string make_get_request(std::string_view path) {
  std::string req = "GET " + std::string(path) +
                    " HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "User-Agent: qps-bench\r\n"
                    "Accept: */*\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n";
  return req;
}

std::string make_post_request(std::string_view path, std::string_view body) {
  std::string req = "POST " + std::string(path) +
                    " HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: " +
                    std::to_string(body.size()) +
                    "\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n";
  req += body;
  return req;
}

} // namespace

int main() {
  auto levels = hps::bench::default_qps_levels();

  // GET short path
  {
    std::string req = make_get_request("/api/health");
    hps::bench::run_qps_steps("HttpParser GET /api/health", levels, [&req](int) {
      thread_local hps::HttpParser parser;
      parser.feed(req);
      parser.reset();
    });
  }

  // POST small body
  {
    std::string req = make_post_request("/api/users", R"({"key":"value","count":42})");
    hps::bench::run_qps_steps("HttpParser POST small body", levels, [&req](int) {
      thread_local hps::HttpParser parser;
      parser.feed(req);
      parser.reset();
    });
  }

  // Many headers
  {
    std::string req = "GET /api/data HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "User-Agent: benchmark-test/1.0\r\n"
                      "Accept: text/html,application/json\r\n"
                      "Accept-Language: en-US,en;q=0.9\r\n"
                      "Accept-Encoding: gzip, deflate\r\n"
                      "Connection: keep-alive\r\n"
                      "Cache-Control: no-cache\r\n"
                      "X-Custom-Header: some-value-here\r\n"
                      "X-Request-ID: abcdef-123456-7890\r\n"
                      "Authorization: Bearer token123\r\n"
                      "\r\n";
    hps::bench::run_qps_steps("HttpParser Many Headers", levels, [&req](int) {
      thread_local hps::HttpParser parser;
      parser.feed(req);
      parser.reset();
    });
  }

  return 0;
}
