#pragma once

#include "i_file_system.h"
#include "models.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hps {

namespace stream_download_detail {

inline bool is_rfc5987_attr_char(unsigned char character) {
  constexpr std::string_view kPunctuation = "!#$&+-.^_`|~";
  return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
         (character >= '0' && character <= '9') ||
         kPunctuation.find(static_cast<char>(character)) != std::string_view::npos;
}

inline char hexadecimal_digit(unsigned char value) {
  constexpr std::string_view kHexDigits = "0123456789ABCDEF";
  return kHexDigits[value & 0x0FU];
}

} // namespace stream_download_detail

inline std::string sanitize_ascii_filename(std::string_view filename) {
  std::string fallback;
  fallback.reserve(filename.size());

  for (const unsigned char character : filename) {
    const bool printable_ascii = character >= 0x20U && character <= 0x7EU;
    const bool unsafe_quoted_string_character = character == '"' || character == '\\' || character == '/';
    fallback.push_back(printable_ascii && !unsafe_quoted_string_character ? static_cast<char>(character) : '_');
  }

  if (fallback.empty() || fallback == "." || fallback == "..") {
    return "download";
  }
  return fallback;
}

inline std::string encode_rfc5987_filename(std::string_view filename) {
  std::string encoded;
  encoded.reserve(filename.size());

  for (const unsigned char character : filename) {
    if (stream_download_detail::is_rfc5987_attr_char(character)) {
      encoded.push_back(static_cast<char>(character));
      continue;
    }
    encoded.push_back('%');
    encoded.push_back(stream_download_detail::hexadecimal_digit(character >> 4U));
    encoded.push_back(stream_download_detail::hexadecimal_digit(character));
  }

  return encoded;
}

inline std::string build_attachment_content_disposition(std::string_view filename) {
  return "attachment; filename=\"" + sanitize_ascii_filename(filename) + "\"; filename*=UTF-8''" +
         encode_rfc5987_filename(filename);
}

inline bool read_stream_file(IFileSystem& fs,
                             const std::vector<FileChunkRecord>& chunks,
                             std::size_t file_size,
                             std::string& out_body) {
  out_body.reserve(file_size);
  for (const auto& chunk : chunks) {
    const auto data = fs.read_file("chunks/" + chunk.chunk_hash);
    if (!data) {
      return false;
    }
    out_body.append(data->data(), data->size());
  }
  return true;
}

inline bool read_stream_range(IFileSystem& fs,
                              const std::vector<FileChunkRecord>& chunks,
                              std::size_t range_start,
                              std::size_t range_end,
                              std::string& out_body) {
  if (range_end < range_start) {
    return false;
  }

  out_body.reserve(range_end - range_start);
  auto remain = range_end - range_start;
  auto chunk_offset = range_start;
  for (const auto& chunk : chunks) {
    if (remain == 0) {
      break;
    }
    if (chunk.chunk_size <= 0) {
      return false;
    }

    const auto chunk_size = static_cast<std::size_t>(chunk.chunk_size);
    if (chunk_offset >= chunk_size) {
      chunk_offset -= chunk_size;
      continue;
    }

    const auto data = fs.read_file("chunks/" + chunk.chunk_hash);
    if (!data || chunk_offset >= data->size()) {
      return false;
    }

    const auto count = std::min<std::size_t>(data->size() - chunk_offset, remain);
    out_body.append(data->data() + chunk_offset, count);
    remain -= count;
    chunk_offset = 0;
  }
  return remain == 0;
}

} // namespace hps
