#include "range_parser.h"

#include <cctype>
#include <charconv>
#include <optional>

namespace hps {

namespace {

bool consume_prefix(std::string_view& header, std::string_view prefix) {
  if (header.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(header[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  header.remove_prefix(prefix.size());
  return true;
}

void skip_spaces(std::string_view& s) {
  while (!s.empty() && (s[0] == ' ' || s[0] == '\t')) {
    s.remove_prefix(1);
  }
}

std::optional<std::size_t> parse_number(std::string_view& s) {
  if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0])) == 0) {
    return std::nullopt;
  }
  std::size_t val = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
  if (ec != std::errc()) {
    return std::nullopt;
  }
  auto consumed = static_cast<std::size_t>(ptr - s.data());
  s.remove_prefix(consumed);
  return val;
}

bool try_parse_suffix(std::string_view& header, std::size_t file_size, RangeInterval& out) {
  if (header.empty() || header[0] != '-') {
    return false;
  }
  header.remove_prefix(1);
  auto suffix = parse_number(header);
  if (!suffix.has_value() || suffix.value() == 0) {
    return false;
  }
  std::size_t start = (suffix.value() >= file_size) ? 0 : (file_size - suffix.value());
  out = {start, file_size};
  return true;
}

bool try_parse_byte_range(std::string_view& header, std::size_t file_size, RangeInterval& out) {
  auto start = parse_number(header);
  if (!start.has_value()) {
    return false;
  }
  if (header.empty() || header[0] != '-') {
    return false;
  }
  header.remove_prefix(1);
  if (header.empty() || header[0] == ',' || header[0] == ' ' || header[0] == '\t') {
    out = {start.value(), file_size};
    return true;
  }
  auto end = parse_number(header);
  if (!end.has_value() || end.value() < start.value()) {
    return false;
  }
  std::size_t clamped_end = (end.value() + 1 >= file_size) ? file_size : (end.value() + 1);
  out = {start.value(), clamped_end};
  return true;
}

} // namespace

RangeRequest parse_range_header(std::string_view header, std::size_t file_size) {
  RangeRequest result;
  if (header.empty()) {
    result.valid = false;
    return result;
  }
  if (!consume_prefix(header, "bytes=")) {
    result.valid = false;
    return result;
  }

  bool any_satisfiable = false;

  while (true) {
    skip_spaces(header);
    if (header.empty()) {
      result.valid = false;
      break;
    }

    RangeInterval iv{0, 0};
    bool parsed = false;

    if (header[0] == '-') {
      parsed = try_parse_suffix(header, file_size, iv);
    } else {
      parsed = try_parse_byte_range(header, file_size, iv);
    }

    if (!parsed) {
      result.valid = false;
      break;
    }

    if (iv.start < file_size) {
      any_satisfiable = true;
    }
    if (iv.start >= file_size) {
      iv = {file_size, file_size};
    }
    result.ranges.push_back(iv);

    skip_spaces(header);
    if (header.empty()) {
      break;
    }
    if (header[0] == ',') {
      header.remove_prefix(1);
      continue;
    }
    result.valid = false;
    break;
  }

  if (!any_satisfiable) {
    result.satisfiable = false;
  }
  return result;
}

void build_206_headers(HttpResponse& resp, const RangeRequest& range, std::size_t file_size) {
  resp.set_status(206, "Partial Content");
  if (!range.satisfiable || range.ranges.empty()) {
    build_416_response(resp, file_size);
    return;
  }
  if (range.ranges.size() == 1) {
    const auto& iv = range.ranges[0];
    auto content_range =
      "bytes " + std::to_string(iv.start) + "-" + std::to_string(iv.end - 1) + "/" + std::to_string(file_size);
    resp.set_header("Content-Range", content_range);
    resp.set_content_length(iv.end - iv.start);
  }
}

void build_416_response(HttpResponse& resp, std::size_t file_size) {
  resp.set_status(416, "Range Not Satisfiable");
  resp.set_header("Content-Range", "bytes */" + std::to_string(file_size));
  resp.set_content_type("text/plain; charset=utf-8");
  resp.body = "416 Range Not Satisfiable";
  resp.set_content_length(resp.body.size());
}

} // namespace hps
