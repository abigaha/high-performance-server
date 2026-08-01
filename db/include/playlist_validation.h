#pragma once

#include <cstddef>
#include <string_view>

namespace hps {

inline bool has_utf8_code_point_length(std::string_view value, std::size_t minimum, std::size_t maximum) noexcept {
  std::size_t count = 0;
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<unsigned char>(value[index]);
    std::size_t width = 0;
    unsigned int code_point = 0;
    if (first <= 0x7F) {
      width = 1;
      code_point = first;
    } else if (first >= 0xC2 && first <= 0xDF) {
      width = 2;
      code_point = first & 0x1FU;
    } else if (first >= 0xE0 && first <= 0xEF) {
      width = 3;
      code_point = first & 0x0FU;
    } else if (first >= 0xF0 && first <= 0xF4) {
      width = 4;
      code_point = first & 0x07U;
    } else {
      return false;
    }
    if (index + width > value.size())
      return false;
    for (std::size_t offset = 1; offset < width; ++offset) {
      const auto continuation = static_cast<unsigned char>(value[index + offset]);
      if ((continuation & 0xC0U) != 0x80U)
        return false;
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if ((width == 3 && code_point < 0x800U) || (width == 4 && code_point < 0x10000U) ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU) || code_point > 0x10FFFFU) {
      return false;
    }
    ++count;
    if (count > maximum)
      return false;
    index += width;
  }
  return count >= minimum;
}

inline bool is_valid_playlist_text(std::string_view name, std::string_view description) noexcept {
  return has_utf8_code_point_length(name, 1, 128) && has_utf8_code_point_length(description, 0, 512);
}

} // namespace hps
