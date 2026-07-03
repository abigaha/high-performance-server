#pragma once

#include "http_response.h"

#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace hps {

struct RangeInterval {
  std::size_t start;
  std::size_t end;
};

struct RangeRequest {
  std::vector<RangeInterval> ranges;
  bool valid{true};
  bool satisfiable{true};
};

RangeRequest parse_range_header(std::string_view header, std::size_t file_size);

void build_206_headers(HttpResponse& resp, const RangeRequest& range, std::size_t file_size);

void build_416_response(HttpResponse& resp, std::size_t file_size);

inline std::string generate_boundary() {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string b = "HPS_";
  thread_local std::mt19937 gen(std::random_device{}());
  std::uniform_int_distribution<int> dist(0, 15);
  for (int i = 0; i < 16; ++i) {
    b += kHex[dist(gen)];
  }
  return b;
}

} // namespace hps
