#include "http_request.h"
#include "http_response.h"
#include "qps_runner.hpp"
#include "router.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct RouterWithRoutes {
  hps::Router router;

  RouterWithRoutes() {
    for (int i = 0; i < 100; ++i) {
      std::string path = "/api/endpoint/" + std::to_string(i);
      router.add(hps::HttpMethod::GET, path, [](const hps::HttpRequest&, hps::HttpResponse&) {});
    }
    router.add(hps::HttpMethod::GET, "/api/users/:id", [](const hps::HttpRequest&, hps::HttpResponse&) {});
    router.add(hps::HttpMethod::POST, "/api/users", [](const hps::HttpRequest&, hps::HttpResponse&) {});
    router.add(hps::HttpMethod::GET, "/api/files/:hash/download", [](const hps::HttpRequest&, hps::HttpResponse&) {});
  }
};

} // namespace

int main() {
  auto levels = hps::bench::default_qps_levels();

  // Static match
  {
    RouterWithRoutes r;
    hps::bench::run_qps_steps("Router Static Match", levels, [&r](int) {
      thread_local hps::Router::Handler handler;
      thread_local std::unordered_map<std::string, std::string> params;
      r.router.match(hps::HttpMethod::GET, "/api/endpoint/42", handler, params);
    });
  }

  // Param match
  {
    RouterWithRoutes r;
    hps::bench::run_qps_steps("Router Param Match", levels, [&r](int) {
      thread_local hps::Router::Handler handler;
      thread_local std::unordered_map<std::string, std::string> params;
      r.router.match(hps::HttpMethod::GET, "/api/users/98765", handler, params);
    });
  }

  // Large route table (5000 routes)
  {
    struct LargeRouter {
      hps::Router router;

      LargeRouter() {
        for (int i = 0; i < 5000; ++i) {
          std::string path = "/api/endpoint/" + std::to_string(i);
          router.add(hps::HttpMethod::GET, path, [](const hps::HttpRequest&, hps::HttpResponse&) {});
        }
      }
    };

    LargeRouter r;
    hps::bench::run_qps_steps("Router 5000 Routes", levels, [&r](int) {
      thread_local hps::Router::Handler handler;
      thread_local std::unordered_map<std::string, std::string> params;
      r.router.match(hps::HttpMethod::GET, "/api/endpoint/4999", handler, params);
    });
  }

  return 0;
}
