#pragma once

#include "models.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace hps {

inline bool is_valid_email(std::string_view email) noexcept {
  if (email.empty() || email.size() > kMaximumEmailLength ||
      std::ranges::any_of(email, [](unsigned char byte) { return std::isspace(byte) != 0; })) {
    return false;
  }
  const auto separator = email.find('@');
  if (separator == std::string_view::npos || separator == 0 || separator + 1 >= email.size() ||
      email.find('@', separator + 1) != std::string_view::npos) {
    return false;
  }
  const auto domain = email.substr(separator + 1);
  const auto dot = domain.find('.');
  return dot != std::string_view::npos && dot > 0 && dot + 1 < domain.size();
}

} // namespace hps
