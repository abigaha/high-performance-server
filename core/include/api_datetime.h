#pragma once

#include "iconnection.h"

#include <optional>
#include <string>
#include <string_view>

namespace hps {

inline std::optional<std::string> format_api_datetime(std::string_view mysql_datetime) {
  const auto parsed = parse_mysql_utc_datetime(mysql_datetime);
  if (!parsed) {
    return std::nullopt;
  }
  return format_rfc3339_utc(*parsed);
}

} // namespace hps
