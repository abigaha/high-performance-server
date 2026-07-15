#include "http_request.h"
#include "http_response.h"
#include "router.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

struct RouterWithRoutes {
  hps::Router router;

  explicit RouterWithRoutes(std::size_t num_routes) {
    for (std::size_t i = 0; i < num_routes; ++i) {
      std::string path = "/api/endpoint/" + std::to_string(i);
      router.add(hps::HttpMethod::GET, path, [](const hps::HttpRequest&, hps::HttpResponse&) {});
    }
    router.add(hps::HttpMethod::GET, "/api/users/:id", [](const hps::HttpRequest&, hps::HttpResponse&) {});
    router.add(hps::HttpMethod::POST, "/api/users", [](const hps::HttpRequest&, hps::HttpResponse&) {});
    router.add(hps::HttpMethod::GET, "/api/files/:hash/download", [](const hps::HttpRequest&, hps::HttpResponse&) {});
    router.add(hps::HttpMethod::POST, "/api/files/upload", [](const hps::HttpRequest&, hps::HttpResponse&) {});
  }
};

} // namespace

static void BM_Router_MatchStatic(benchmark::State& state) {
  RouterWithRoutes r(100);
  hps::Router::Handler handler;
  std::unordered_map<std::string, std::string> params;
  for (auto _ : state) {
    auto ok = r.router.match(hps::HttpMethod::GET, "/api/endpoint/42", handler, params);
    benchmark::DoNotOptimize(ok);
    benchmark::DoNotOptimize(handler);
  }
}

BENCHMARK(BM_Router_MatchStatic);

static void BM_Router_MatchParam(benchmark::State& state) {
  RouterWithRoutes r(100);
  hps::Router::Handler handler;
  std::unordered_map<std::string, std::string> params;
  for (auto _ : state) {
    auto ok = r.router.match(hps::HttpMethod::GET, "/api/users/98765", handler, params);
    benchmark::DoNotOptimize(ok);
    benchmark::DoNotOptimize(params);
  }
}

BENCHMARK(BM_Router_MatchParam);

static void BM_Router_MatchNotFound(benchmark::State& state) {
  RouterWithRoutes r(100);
  hps::Router::Handler handler;
  std::unordered_map<std::string, std::string> params;
  for (auto _ : state) {
    auto ok = r.router.match(hps::HttpMethod::GET, "/api/notfound/xyz", handler, params);
    benchmark::DoNotOptimize(ok);
  }
}

BENCHMARK(BM_Router_MatchNotFound);

static void BM_Router_PathExists(benchmark::State& state) {
  RouterWithRoutes r(100);
  for (auto _ : state) {
    auto ok = r.router.path_exists("/api/endpoint/42");
    benchmark::DoNotOptimize(ok);
  }
}

BENCHMARK(BM_Router_PathExists);

static void BM_Router_MatchLongPath(benchmark::State& state) {
  RouterWithRoutes r(10);
  hps::Router::Handler handler;
  std::unordered_map<std::string, std::string> params;
  for (auto _ : state) {
    auto ok = r.router.match(hps::HttpMethod::GET, "/api/users/12345/documents/details", handler, params);
    benchmark::DoNotOptimize(ok);
  }
}

BENCHMARK(BM_Router_MatchLongPath);

static void BM_Router_MatchLargeTable(benchmark::State& state) {
  RouterWithRoutes r(5000);
  hps::Router::Handler handler;
  std::unordered_map<std::string, std::string> params;
  std::string target = "/api/endpoint/" + std::to_string(static_cast<std::size_t>(4999));
  for (auto _ : state) {
    auto ok = r.router.match(hps::HttpMethod::GET, target, handler, params);
    benchmark::DoNotOptimize(ok);
  }
}

BENCHMARK(BM_Router_MatchLargeTable);

BENCHMARK_MAIN();
