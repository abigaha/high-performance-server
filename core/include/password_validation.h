#pragma once

#include <cstddef>
#include <string_view>

namespace hps {

inline constexpr std::size_t kMinimumUserPasswordBytes = 6;
inline constexpr std::size_t kMaximumUserPasswordBytes = 128;

inline bool is_valid_user_password(std::string_view password) noexcept {
  return password.size() >= kMinimumUserPasswordBytes && password.size() <= kMaximumUserPasswordBytes;
}

} // namespace hps
