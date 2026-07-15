#include "http_parser.h"
#include "http_request.h"

#include <benchmark/benchmark.h>

#include <string>
#include <string_view>

namespace {

std::string make_get_request(std::string_view path) {
  std::string req = "GET " + std::string(path) +
                    " HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "User-Agent: benchmark\r\n"
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

static void BM_HttpParser_GetShortPath(benchmark::State& state) {
  auto req = make_get_request("/api/health");
  for (auto _ : state) {
    hps::HttpParser parser;
    auto result = parser.feed(req);
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK(BM_HttpParser_GetShortPath);

static void BM_HttpParser_GetLongPath(benchmark::State& state) {
  std::string long_path = "/api/users/1234567890abcdef/documents/details";
  auto req = make_get_request(long_path);
  for (auto _ : state) {
    hps::HttpParser parser;
    auto result = parser.feed(req);
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK(BM_HttpParser_GetLongPath);

static void BM_HttpParser_PostSmallBody(benchmark::State& state) {
  std::string body = R"({"key":"value","count":42})";
  auto req = make_post_request("/api/users", body);
  for (auto _ : state) {
    hps::HttpParser parser;
    auto result = parser.feed(req);
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK(BM_HttpParser_PostSmallBody);

static void BM_HttpParser_PostLargeBody(benchmark::State& state) {
  std::string body(state.range(0), 'x');
  auto req = make_post_request("/api/upload", body);
  for (auto _ : state) {
    hps::HttpParser parser;
    parser.feed(req);
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_HttpParser_PostLargeBody)->Range(1024, 64 * 1024);

static void BM_HttpParser_ResetReuse(benchmark::State& state) {
  auto req = make_get_request("/api/health");
  hps::HttpParser parser;
  for (auto _ : state) {
    parser.feed(req);
    parser.reset();
  }
}

BENCHMARK(BM_HttpParser_ResetReuse);

static void BM_HttpParser_ManyHeaders(benchmark::State& state) {
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
  for (auto _ : state) {
    hps::ParserResult result;
    {
      hps::HttpParser parser;
      result = parser.feed(req);
    }
    benchmark::DoNotOptimize(result);
  }
}

BENCHMARK(BM_HttpParser_ManyHeaders);

BENCHMARK_MAIN();
