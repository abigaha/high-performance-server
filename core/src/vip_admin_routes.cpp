#include "vip_admin_routes.h"

#include "authorization.h"
#include "iconnection.h"
#include "idatabase_pool.h"
#include "models.h"
#include "strict_json.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace hps {

namespace {

HttpResponse json_error(int status, std::string_view code, std::string_view message) {
  HttpResponse response;
  std::string_view status_text;
  switch (status) {
    case 400:
      status_text = "Bad Request";
      break;
    case 401:
      status_text = "Unauthorized";
      break;
    case 403:
      status_text = "Forbidden";
      break;
    case 404:
      status_text = "Not Found";
      break;
    case 409:
      status_text = "Conflict";
      break;
    case 422:
      status_text = "Unprocessable Entity";
      break;
    default:
      status_text = "Internal Server Error";
      break;
  }
  response.set_status(status, status_text);
  response.set_content_type("application/json");
  response.body = nlohmann::json{{"code", code}, {"error", message}}.dump();
  response.set_content_length(response.body.size());
  return response;
}

void set_json_response(HttpResponse& response, const nlohmann::json& body) {
  response.set_status(200, "OK");
  response.set_content_type("application/json");
  response.body = body.dump();
  response.set_content_length(response.body.size());
}

bool require_authenticated(const HttpRequest& request, HttpResponse& response) {
  if (request.auth_status == TokenValidationStatus::STORAGE_ERROR) {
    response = json_error(500, "PERSISTENCE_ERROR", "认证存储暂时不可用");
    return false;
  }
  if (!has_capability(request.auth_user, Capability::USE_AUTHENTICATED_FEATURES)) {
    response = json_error(401, "AUTH_REQUIRED", "需要登录");
    return false;
  }
  return true;
}

bool require_admin(const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response)) {
    return false;
  }
  if (!has_capability(request.auth_user, Capability::MANAGE_USERS)) {
    response = json_error(403, "ADMIN_REQUIRED", "需要管理员权限");
    return false;
  }
  return true;
}

std::string_view role_name(UserRole role) {
  switch (role) {
    case UserRole::NORMAL:
      return "NORMAL";
    case UserRole::VIP:
      return "VIP";
    case UserRole::ADMIN:
      return "ADMIN";
    case UserRole::GUEST:
      return "GUEST";
  }
  return "GUEST";
}

std::string_view vip_status_name(VipStatus status) {
  switch (status) {
    case VipStatus::NONE:
      return "NONE";
    case VipStatus::ACTIVE:
      return "ACTIVE";
    case VipStatus::EXPIRED:
      return "EXPIRED";
  }
  return "NONE";
}

nlohmann::json optional_time_json(const std::optional<std::chrono::system_clock::time_point>& value) {
  if (!value) {
    return nullptr;
  }
  return format_rfc3339_utc(*value);
}

int64_t remaining_seconds(const std::optional<std::chrono::system_clock::time_point>& expires_at,
                          std::chrono::system_clock::time_point now) noexcept {
  if (!expires_at || *expires_at <= now) {
    return 0;
  }
  const auto expiration_count = std::chrono::floor<std::chrono::seconds>(*expires_at).time_since_epoch().count();
  const auto now_count = std::chrono::floor<std::chrono::seconds>(now).time_since_epoch().count();
  constexpr auto kMaximum = std::numeric_limits<int64_t>::max();
  if (!std::in_range<int64_t>(expiration_count) || !std::in_range<int64_t>(now_count)) {
    return kMaximum;
  }
  const auto expiration_seconds = static_cast<int64_t>(expiration_count);
  const auto now_seconds = static_cast<int64_t>(now_count);
  if (expiration_seconds <= now_seconds) {
    return 0;
  }
  if (now_seconds < 0 && expiration_seconds > kMaximum + now_seconds) {
    return kMaximum;
  }
  return expiration_seconds - now_seconds;
}

nlohmann::json membership_json(const User& user, std::chrono::system_clock::time_point now) {
  const auto effective = make_effective_identity(user, now);
  return {{"role", role_name(effective.role)},
          {"vip_status", vip_status_name(effective.vip_status)},
          {"vip_expires_at", optional_time_json(user.vip_expires_at)},
          {"server_now", format_rfc3339_utc(now)},
          {"remaining_seconds", remaining_seconds(user.vip_expires_at, now)}};
}

nlohmann::json admin_user_json(const User& user, std::chrono::system_clock::time_point now) {
  const auto effective = make_effective_identity(user, now);
  const auto created_at = parse_mysql_utc_datetime(user.created_at);
  if (!created_at) {
    throw std::runtime_error("invalid user timestamp");
  }
  return {{"user_id", user.user_id},
          {"username", user.username},
          {"email", user.email},
          {"role", role_name(effective.role)},
          {"vip_status", vip_status_name(effective.vip_status)},
          {"vip_expires_at", optional_time_json(user.vip_expires_at)},
          {"created_at", format_rfc3339_utc(*created_at)}};
}

std::optional<int> parse_duration(const std::string& body, HttpResponse& response) {
  const auto payload = parse_strict_json_object(body, {{"duration_days", StrictJsonValueType::INTEGER, true}});
  if (!payload) {
    response = json_error(400, "INVALID_REQUEST", "请求 JSON 无效");
    return std::nullopt;
  }
  const auto duration_field = payload->find("duration_days");
  if (duration_field == payload->end()) {
    response = json_error(400, "INVALID_REQUEST", "请求 JSON 无效");
    return std::nullopt;
  }
  const auto duration = strict_json_integer_value(*duration_field);
  if (!duration || (*duration != 30 && *duration != 90 && *duration != 365)) {
    response = json_error(400, "INVALID_VIP_DURATION", "会员时长必须是 30、90 或 365 天");
    return std::nullopt;
  }
  return static_cast<int>(*duration);
}

std::optional<int64_t> parse_positive_id(const HttpRequest& request, HttpResponse& response) {
  const auto item = request.path_params.find("id");
  if (item == request.path_params.end() || item->second.empty()) {
    response = json_error(400, "INVALID_REQUEST", "用户 ID 无效");
    return std::nullopt;
  }
  int64_t value = 0;
  const auto* begin = item->second.data();
  const auto* end = begin + item->second.size();
  const auto [position, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || position != end || value <= 0) {
    response = json_error(400, "INVALID_REQUEST", "用户 ID 无效");
    return std::nullopt;
  }
  return value;
}

std::optional<std::string> url_decode(std::string_view value) {
  std::string decoded;
  decoded.reserve(value.size());
  const auto hex_value = [](char character) -> int {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
      return character - 'A' + 10;
    }
    return -1;
  };
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '+') {
      decoded.push_back(' ');
      continue;
    }
    if (value[index] != '%') {
      decoded.push_back(value[index]);
      continue;
    }
    if (index + 2 >= value.size()) {
      return std::nullopt;
    }
    const int high = hex_value(value[index + 1]);
    const int low = hex_value(value[index + 2]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    decoded.push_back(static_cast<char>((static_cast<unsigned>(high) << 4U) | static_cast<unsigned>(low)));
    index += 2;
  }
  return decoded;
}

struct AdminQuery {
  std::string query;
  int offset{0};
  int limit{20};
};

bool parse_nonnegative_integer(std::string_view text, int& value) {
  if (text.empty()) {
    return false;
  }
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [position, error] = std::from_chars(begin, end, value);
  return error == std::errc{} && position == end && value >= 0;
}

std::optional<AdminQuery> parse_admin_query(std::string_view query_string) {
  AdminQuery result;
  std::unordered_map<std::string, std::string> values;
  while (!query_string.empty()) {
    const auto separator = query_string.find('&');
    const auto item = query_string.substr(0, separator);
    query_string = separator == std::string_view::npos ? std::string_view{} : query_string.substr(separator + 1);
    if (item.empty()) {
      return std::nullopt;
    }
    const auto equals = item.find('=');
    if (equals == std::string_view::npos) {
      return std::nullopt;
    }
    auto key = url_decode(item.substr(0, equals));
    auto value = url_decode(item.substr(equals + 1));
    if (!key || !value || (*key != "q" && *key != "offset" && *key != "limit") || values.contains(*key)) {
      return std::nullopt;
    }
    values.emplace(std::move(*key), std::move(*value));
  }
  if (const auto item = values.find("q"); item != values.end()) {
    result.query = item->second;
  }
  if (const auto item = values.find("offset");
      item != values.end() && !parse_nonnegative_integer(item->second, result.offset)) {
    return std::nullopt;
  }
  if (const auto item = values.find("limit"); item != values.end()) {
    if (!parse_nonnegative_integer(item->second, result.limit) || result.limit < 1 || result.limit > 100) {
      return std::nullopt;
    }
  }
  return result;
}

HttpResponse mutation_error(MutationStatus status) {
  switch (status) {
    case MutationStatus::NOT_FOUND:
    case MutationStatus::USER_NOT_FOUND:
      return json_error(404, "USER_NOT_FOUND", "用户不存在");
    case MutationStatus::OWNER_REQUIRED:
      return json_error(403, "ADMIN_REQUIRED", "需要管理员权限");
    case MutationStatus::CONFLICT:
      return json_error(409, "ADMIN_MEMBERSHIP_FORBIDDEN", "不能修改管理员会员状态");
    case MutationStatus::INVALID_STATE:
      return json_error(422, "VIP_STATE_INVALID", "会员数据状态无效");
    case MutationStatus::STORAGE_ERROR:
      return json_error(500, "PERSISTENCE_ERROR", "会员数据存储失败");
    case MutationStatus::OK:
      break;
  }
  return json_error(500, "PERSISTENCE_ERROR", "会员数据存储失败");
}

void handle_get_vip_plans(const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response)) {
    return;
  }
  if (request.auth_user.role == UserRole::ADMIN) {
    response = json_error(403, "VIP_SELF_SERVICE_UNAVAILABLE", "管理员不能使用自助会员服务");
    return;
  }
  set_json_response(response,
                    {{"plans",
                      nlohmann::json::array({{{"duration_days", 30}, {"label", "30 天"}},
                                             {{"duration_days", 90}, {"label", "90 天"}},
                                             {{"duration_days", 365}, {"label", "365 天"}}})}});
}

void handle_get_vip_membership(IDatabasePool& database,
                               VipAdminClock clock,
                               const HttpRequest& request,
                               HttpResponse& response) {
  if (!require_authenticated(request, response)) {
    return;
  }
  if (request.auth_user.role == UserRole::ADMIN) {
    response = json_error(403, "VIP_SELF_SERVICE_UNAVAILABLE", "管理员不能使用自助会员服务");
    return;
  }
  try {
    const auto user = database.get_user_result(request.auth_user.user_id);
    if (user.status == LookupStatus::NOT_FOUND) {
      response = json_error(404, "USER_NOT_FOUND", "用户不存在");
      return;
    }
    if (user.status != LookupStatus::FOUND || !user.value) {
      response = json_error(500, "PERSISTENCE_ERROR", "会员数据读取失败");
      return;
    }
    if (user.value->role == UserRole::ADMIN) {
      response = json_error(403, "VIP_SELF_SERVICE_UNAVAILABLE", "管理员不能使用自助会员服务");
      return;
    }
    if (user.value->role != UserRole::NORMAL && user.value->role != UserRole::VIP) {
      response = json_error(422, "VIP_STATE_INVALID", "会员数据状态无效");
      return;
    }
    set_json_response(response, membership_json(*user.value, clock()));
  } catch (...) {
    response = json_error(500, "PERSISTENCE_ERROR", "会员数据读取失败");
  }
}

void handle_activate_vip(IDatabasePool& database,
                         VipAdminClock clock,
                         const HttpRequest& request,
                         HttpResponse& response) {
  if (!require_authenticated(request, response)) {
    return;
  }
  if (request.auth_user.role == UserRole::ADMIN) {
    response = json_error(403, "VIP_SELF_SERVICE_UNAVAILABLE", "管理员不能使用自助会员服务");
    return;
  }
  const auto duration = parse_duration(request.body, response);
  if (!duration) {
    return;
  }
  try {
    const auto now = clock();
    const auto result = database.grant_or_extend_vip(request.auth_user.user_id, *duration, now);
    if (result.status != MutationStatus::OK || !result.value) {
      response = mutation_error(result.status);
      return;
    }
    set_json_response(response, membership_json(*result.value, now));
  } catch (...) {
    response = json_error(500, "PERSISTENCE_ERROR", "会员数据存储失败");
  }
}

void handle_admin_list_users(IDatabasePool& database,
                             VipAdminClock clock,
                             const HttpRequest& request,
                             HttpResponse& response) {
  if (!require_admin(request, response)) {
    return;
  }
  const auto query = parse_admin_query(request.query_string);
  if (!query) {
    response = json_error(400, "INVALID_REQUEST", "查询参数无效");
    return;
  }
  try {
    const auto users = database.list_admin_users(query->query, query->offset, query->limit);
    if (users.status != LookupStatus::FOUND || !users.value) {
      response = json_error(500, "PERSISTENCE_ERROR", "用户列表读取失败");
      return;
    }
    const auto now = clock();
    nlohmann::json items = nlohmann::json::array();
    std::transform(users.value->items.begin(),
                   users.value->items.end(),
                   std::back_inserter(items),
                   [now](const User& user) { return admin_user_json(user, now); });
    set_json_response(response,
                      {{"items", std::move(items)},
                       {"total", users.value->total},
                       {"offset", users.value->offset},
                       {"limit", users.value->limit}});
  } catch (...) {
    response = json_error(500, "PERSISTENCE_ERROR", "用户列表读取失败");
  }
}

void handle_admin_grant_vip(IDatabasePool& database,
                            VipAdminClock clock,
                            const HttpRequest& request,
                            HttpResponse& response) {
  if (!require_admin(request, response)) {
    return;
  }
  const auto user_id = parse_positive_id(request, response);
  if (!user_id) {
    return;
  }
  const auto duration = parse_duration(request.body, response);
  if (!duration) {
    return;
  }
  try {
    const auto now = clock();
    const auto result = database.grant_or_extend_vip(*user_id, *duration, now);
    if (result.status != MutationStatus::OK || !result.value) {
      response = mutation_error(result.status);
      return;
    }
    set_json_response(response, admin_user_json(*result.value, now));
  } catch (...) {
    response = json_error(500, "PERSISTENCE_ERROR", "会员数据存储失败");
  }
}

void handle_admin_revoke_vip(IDatabasePool& database,
                             VipAdminClock clock,
                             const HttpRequest& request,
                             HttpResponse& response) {
  if (!require_admin(request, response)) {
    return;
  }
  const auto user_id = parse_positive_id(request, response);
  if (!user_id) {
    return;
  }
  try {
    const auto result = database.revoke_vip(*user_id);
    if (result.status != MutationStatus::OK || !result.value) {
      response = mutation_error(result.status);
      return;
    }
    set_json_response(response, admin_user_json(*result.value, clock()));
  } catch (...) {
    response = json_error(500, "PERSISTENCE_ERROR", "会员数据存储失败");
  }
}

} // namespace

void register_vip_routes(IHttpServer& server, IDatabasePool& database, VipAdminClock clock) {
  server.get("/api/vip/plans",
             [](const HttpRequest& request, HttpResponse& response) { handle_get_vip_plans(request, response); });
  server.get("/api/vip/membership", [&database, clock](const HttpRequest& request, HttpResponse& response) {
    handle_get_vip_membership(database, clock, request, response);
  });
  server.post("/api/vip/membership/activate", [&database, clock](const HttpRequest& request, HttpResponse& response) {
    handle_activate_vip(database, clock, request, response);
  });
}

void register_admin_routes(IHttpServer& server, IDatabasePool& database, VipAdminClock clock) {
  server.get("/api/admin/users", [&database, clock](const HttpRequest& request, HttpResponse& response) {
    handle_admin_list_users(database, clock, request, response);
  });
  server.post("/api/admin/users/:id/vip", [&database, clock](const HttpRequest& request, HttpResponse& response) {
    handle_admin_grant_vip(database, clock, request, response);
  });
  server.del("/api/admin/users/:id/vip", [&database, clock](const HttpRequest& request, HttpResponse& response) {
    handle_admin_revoke_vip(database, clock, request, response);
  });
}

} // namespace hps
