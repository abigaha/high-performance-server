#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace hps {

enum class StrictJsonValueType : uint8_t { STRING, INTEGER, ARRAY };
using StrictJsonFieldType = StrictJsonValueType;

struct StrictJsonField {
  std::string_view name;
  StrictJsonValueType type;
  bool required;
  bool nonnegative{false};
};

inline std::optional<int64_t> strict_json_integer_value(const nlohmann::json& value) {
  if (value.is_number_unsigned()) {
    const auto unsigned_value = value.get<uint64_t>();
    if (unsigned_value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<int64_t>(unsigned_value);
  }
  if (value.is_number_integer()) {
    return value.get<int64_t>();
  }
  return std::nullopt;
}

inline bool matches_strict_json_type(const nlohmann::json& value, StrictJsonValueType type, bool nonnegative = false) {
  switch (type) {
    case StrictJsonValueType::STRING:
      return value.is_string();
    case StrictJsonValueType::INTEGER: {
      const auto integer = strict_json_integer_value(value);
      return integer && (!nonnegative || *integer >= 0);
    }
    case StrictJsonValueType::ARRAY:
      return value.is_array();
  }
  return false;
}

inline std::optional<nlohmann::json> parse_strict_json_object(std::string_view body) {
  try {
    bool duplicate_key = false;
    std::unordered_map<int, std::unordered_set<std::string>> keys_by_depth;
    const auto callback = [&duplicate_key,
                           &keys_by_depth](int depth, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
      if (event == nlohmann::json::parse_event_t::object_start) {
        keys_by_depth[depth + 1].clear();
      } else if (event == nlohmann::json::parse_event_t::key &&
                 !keys_by_depth[depth].insert(parsed.get<std::string>()).second) {
        duplicate_key = true;
      } else if (event == nlohmann::json::parse_event_t::object_end) {
        keys_by_depth.erase(depth + 1);
      }
      return true;
    };
    auto payload = nlohmann::json::parse(body, callback);
    if (duplicate_key || !payload.is_object())
      return std::nullopt;
    return payload;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

inline bool matches_strict_json_object(const nlohmann::json& payload, std::initializer_list<StrictJsonField> fields) {
  if (!payload.is_object()) {
    return false;
  }
  for (const auto& field : fields) {
    const auto item = payload.find(field.name);
    if (item == payload.end()) {
      if (field.required) {
        return false;
      }
      continue;
    }
    if (!matches_strict_json_type(*item, field.type, field.nonnegative)) {
      return false;
    }
  }
  for (const auto& item : payload.items()) {
    const bool known =
      std::ranges::any_of(fields, [&item](const StrictJsonField& field) { return item.key() == field.name; });
    if (!known) {
      return false;
    }
  }
  return true;
}

inline std::optional<nlohmann::json> parse_strict_json_object(std::string_view body,
                                                              std::initializer_list<StrictJsonField> fields) {
  auto payload = parse_strict_json_object(body);
  if (!payload || !matches_strict_json_object(*payload, fields)) {
    return std::nullopt;
  }
  return payload;
}

} // namespace hps
