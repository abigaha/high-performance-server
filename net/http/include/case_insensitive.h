#pragma once

#include <cctype>
#include <string>
#include <unordered_map>

namespace hps {

/** 大小写不敏感的字符串哈希 */
struct CaseInsensitiveHash {
  size_t operator()(const std::string& key) const noexcept {
    size_t h = 0;
    for (char ch : key) {
      // 将字符转为大写后参与哈希计算
      auto upper = static_cast<size_t>(std::toupper(static_cast<unsigned char>(ch)));
      h ^= upper + static_cast<size_t>(0x9e3779b9) + (h << 6U) + (h >> 2U);
    }
    return h;
  }
};

/** 大小写不敏感的字符串等值比较 */
struct CaseInsensitiveEq {
  bool operator()(const std::string& lhs, const std::string& rhs) const noexcept {
    if (lhs.size() != rhs.size())
      return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
      if (std::toupper(static_cast<unsigned char>(lhs[i])) != std::toupper(static_cast<unsigned char>(rhs[i]))) {
        return false;
      }
    }
    return true;
  }
};

/**
 * 大小写不敏感的 Header Map
 *
 * 用于 HTTP 头，使得 "Content-Type"、"content-type"、"CONTENT-TYPE" 等
 * 形式均视为同一键。
 */
using HeaderMap = std::unordered_map<std::string, std::string, CaseInsensitiveHash, CaseInsensitiveEq>;

} // namespace hps
