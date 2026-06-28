#include "url_decode.h"

namespace hps {

namespace {

int hex_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

} // anonymous namespace

bool url_decode(const std::string& src, std::string& dst) {
  dst.clear();
  dst.reserve(src.size());
  for (size_t i = 0; i < src.size(); ++i) {
    if (src[i] == '%') {
      if (i + 2 >= src.size())
        return false;

      int hi = hex_value(src[i + 1]);
      int lo = hex_value(src[i + 2]);
      if (hi < 0 || lo < 0)
        return false;
      dst.push_back(static_cast<char>((static_cast<unsigned>(hi) << 4U) | static_cast<unsigned>(lo)));
      i += 2;
    } else if (src[i] == '+') {
      dst.push_back(' ');
    } else {
      dst.push_back(src[i]);
    }
  }
  return true;
}

} // namespace hps
