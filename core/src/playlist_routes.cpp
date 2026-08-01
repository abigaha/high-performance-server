#include "playlist_routes.h"

#include "api_datetime.h"
#include "authorization.h"
#include "i_http_server.h"
#include "idatabase_pool.h"
#include "playlist_validation.h"
#include "strict_json.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hps {

std::optional<nlohmann::json> parse_playlist_json_object(std::string_view body) {
  return parse_strict_json_object(body);
}

namespace {

HttpResponse json_error(int status, std::string_view code, std::string_view message) {
  HttpResponse response;
  std::string_view reason;
  switch (status) {
    case 400:
      reason = "Bad Request";
      break;
    case 401:
      reason = "Unauthorized";
      break;
    case 403:
      reason = "Forbidden";
      break;
    case 404:
      reason = "Not Found";
      break;
    case 409:
      reason = "Conflict";
      break;
    case 422:
      reason = "Unprocessable Entity";
      break;
    default:
      reason = "Internal Server Error";
      break;
  }
  response.set_status(status, reason);
  response.set_content_type("application/json");
  response.body = nlohmann::json{{"code", code}, {"error", message}}.dump();
  response.set_content_length(response.body.size());
  return response;
}

HttpResponse invalid_time_error() {
  return json_error(422, "INVALID_STATE", "歌单时间数据无效");
}

void set_json(HttpResponse& response, int status, std::string_view reason, const nlohmann::json& body) {
  response.set_status(status, reason);
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

std::optional<int64_t> parse_id(const HttpRequest& request, std::string_view key, HttpResponse& response) {
  const auto item = request.path_params.find(std::string(key));
  if (item == request.path_params.end() || item->second.empty()) {
    response = json_error(400, "INVALID_REQUEST", "ID 无效");
    return std::nullopt;
  }
  int64_t value = 0;
  const auto* begin = item->second.data();
  const auto* end = begin + item->second.size();
  const auto [position, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || position != end || value <= 0) {
    response = json_error(400, "INVALID_REQUEST", "ID 无效");
    return std::nullopt;
  }
  return value;
}

HttpResponse mutation_error(MutationStatus status, const std::optional<std::string>& detail) {
  if (status == MutationStatus::NOT_FOUND && detail == "MUSIC_NOT_FOUND")
    return json_error(404, "MUSIC_NOT_FOUND", "歌曲不存在");
  if (status == MutationStatus::USER_NOT_FOUND || detail == "USER_NOT_FOUND")
    return json_error(404, "USER_NOT_FOUND", "用户不存在");
  switch (status) {
    case MutationStatus::NOT_FOUND:
    case MutationStatus::USER_NOT_FOUND:
      return json_error(404, "PLAYLIST_NOT_FOUND", "歌单不存在");
    case MutationStatus::OWNER_REQUIRED:
      return json_error(403, "PLAYLIST_OWNER_REQUIRED", "仅歌单所有者可执行此操作");
    case MutationStatus::INVALID_STATE:
      return json_error(400, "INVALID_REQUEST", "请求内容无效");
    case MutationStatus::CONFLICT:
      return json_error(409, "PLAYLIST_ORDER_CONFLICT", "歌单状态冲突");
    case MutationStatus::STORAGE_ERROR:
      return json_error(500, "PERSISTENCE_ERROR", "歌单存储暂时不可用");
    case MutationStatus::OK:
      break;
  }
  return json_error(500, "PERSISTENCE_ERROR", "歌单存储暂时不可用");
}

std::optional<nlohmann::json> playlist_json(const Playlist& playlist) {
  const auto created_at = format_api_datetime(playlist.created_at);
  if (!created_at) {
    return std::nullopt;
  }
  return nlohmann::json{{"id", playlist.playlist_id},
                        {"user_id", playlist.user_id},
                        {"name", playlist.name},
                        {"description", playlist.description},
                        {"item_count", playlist.item_count},
                        {"created_at", *created_at}};
}

std::optional<nlohmann::json> item_json(const PlaylistItem& item) {
  const auto added_at = format_api_datetime(item.added_at);
  if (!added_at) {
    return std::nullopt;
  }
  return nlohmann::json{{"id", item.id},
                        {"playlist_id", item.playlist_id},
                        {"music_id", item.music_id},
                        {"title", item.title},
                        {"artist", item.artist},
                        {"file_hash", item.file_hash},
                        {"sort_order", item.sort_order},
                        {"added_at", *added_at}};
}

std::optional<nlohmann::json> parse_object(const std::string& body, HttpResponse& response) {
  auto payload = parse_playlist_json_object(body);
  if (!payload)
    response = json_error(400, "INVALID_REQUEST", "请求 JSON 无效或包含重复字段");
  return payload;
}

bool has_only_fields(const nlohmann::json& payload, std::initializer_list<std::string_view> allowed) {
  if (payload.size() > allowed.size())
    return false;
  return std::ranges::all_of(payload.items(), [allowed](const auto& item) {
    return std::ranges::any_of(allowed, [&item](std::string_view field) { return item.key() == field; });
  });
}

} // namespace

void handle_get_user_playlists(IDatabasePool& database, const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response))
    return;
  const auto user_id = parse_id(request, "id", response);
  if (!user_id)
    return;
  const auto result = database.get_user_playlists(*user_id, request.auth_user.user_id);
  if (result.status != MutationStatus::OK || !result.value) {
    response = mutation_error(result.status, result.detail);
    return;
  }
  nlohmann::json playlists = nlohmann::json::array();
  for (const auto& playlist : *result.value) {
    auto item = playlist_json(playlist);
    if (!item) {
      response = invalid_time_error();
      return;
    }
    playlists.push_back(std::move(*item));
  }
  set_json(response, 200, "OK", {{"playlists", std::move(playlists)}});
}

void handle_create_playlist(IDatabasePool& database, const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response))
    return;
  const auto user_id = parse_id(request, "id", response);
  if (!user_id)
    return;
  const auto payload = parse_object(request.body, response);
  if (!payload || !has_only_fields(*payload, {"name", "description"}) || !payload->contains("name") ||
      !payload->at("name").is_string() ||
      (payload->contains("description") && !payload->at("description").is_string())) {
    if (payload)
      response = json_error(400, "INVALID_REQUEST", "歌单名称无效");
    return;
  }
  Playlist playlist;
  playlist.user_id = *user_id;
  playlist.name = payload->at("name").get<std::string>();
  playlist.description = payload->value("description", "");
  if (!is_valid_playlist_text(playlist.name, playlist.description)) {
    response = json_error(400, "INVALID_REQUEST", "歌单名称或描述长度无效");
    return;
  }
  const auto result = database.create_playlist(playlist, request.auth_user.user_id);
  if (result.status != MutationStatus::OK || !result.value) {
    response = mutation_error(result.status, result.detail);
    return;
  }
  const auto body = playlist_json(*result.value);
  if (!body) {
    response = invalid_time_error();
    return;
  }
  set_json(response, 201, "Created", *body);
}

void handle_update_playlist(IDatabasePool& database, const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response))
    return;
  const auto playlist_id = parse_id(request, "id", response);
  if (!playlist_id)
    return;
  const auto payload = parse_object(request.body, response);
  if (!payload || !has_only_fields(*payload, {"name", "description"}) || !payload->contains("name") ||
      !payload->at("name").is_string() ||
      (payload->contains("description") && !payload->at("description").is_string())) {
    if (payload)
      response = json_error(400, "INVALID_REQUEST", "歌单内容无效");
    return;
  }
  const auto name = payload->at("name").get<std::string>();
  const auto description = payload->value("description", "");
  if (!is_valid_playlist_text(name, description)) {
    response = json_error(400, "INVALID_REQUEST", "歌单名称或描述长度无效");
    return;
  }
  const auto result = database.update_playlist(*playlist_id, request.auth_user.user_id, name, description);
  if (result.status != MutationStatus::OK || !result.value) {
    response = mutation_error(result.status, result.detail);
    return;
  }
  const auto body = playlist_json(*result.value);
  if (!body) {
    response = invalid_time_error();
    return;
  }
  set_json(response, 200, "OK", *body);
}

void handle_delete_playlist(IDatabasePool& database, const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response))
    return;
  const auto playlist_id = parse_id(request, "id", response);
  if (!playlist_id)
    return;
  const auto result = database.delete_playlist(*playlist_id, request.auth_user.user_id);
  if (result.status != MutationStatus::OK) {
    response = mutation_error(result.status, result.detail);
    return;
  }
  response.set_status(204, "No Content");
  response.set_content_length(0);
}

void handle_get_playlist_items(IDatabasePool& database, const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response))
    return;
  const auto playlist_id = parse_id(request, "id", response);
  if (!playlist_id)
    return;
  const auto result = database.get_playlist_items(*playlist_id, request.auth_user.user_id);
  if (result.status != MutationStatus::OK || !result.value) {
    response = mutation_error(result.status, result.detail);
    return;
  }
  nlohmann::json items = nlohmann::json::array();
  for (const auto& item : *result.value) {
    auto serialized = item_json(item);
    if (!serialized) {
      response = invalid_time_error();
      return;
    }
    items.push_back(std::move(*serialized));
  }
  set_json(response, 200, "OK", {{"playlist_id", *playlist_id}, {"items", std::move(items)}});
}

void handle_add_playlist_item(IDatabasePool& database, const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response))
    return;
  const auto playlist_id = parse_id(request, "id", response);
  if (!playlist_id)
    return;
  const auto payload = parse_object(request.body, response);
  if (!payload || !has_only_fields(*payload, {"music_id"}) || !payload->contains("music_id") ||
      !strict_json_integer_value(payload->at("music_id"))) {
    if (payload)
      response = json_error(400, "INVALID_REQUEST", "歌曲 ID 无效");
    return;
  }
  const auto music_id = strict_json_integer_value(payload->at("music_id"));
  if (!music_id || *music_id <= 0) {
    response = json_error(400, "INVALID_REQUEST", "歌曲 ID 无效");
    return;
  }
  const auto result = database.add_playlist_item(*playlist_id, request.auth_user.user_id, *music_id);
  if (result.status != MutationStatus::OK) {
    response = mutation_error(result.status, result.detail);
    return;
  }
  set_json(response, 201, "Created", {{"message", "已添加"}});
}

void handle_remove_playlist_item(IDatabasePool& database, const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response))
    return;
  const auto playlist_id = parse_id(request, "id", response);
  const auto music_id = parse_id(request, "music_id", response);
  if (!playlist_id || !music_id)
    return;
  const auto result = database.remove_playlist_item(*playlist_id, request.auth_user.user_id, *music_id);
  if (result.status != MutationStatus::OK) {
    response = mutation_error(result.status, result.detail);
    return;
  }
  response.set_status(204, "No Content");
  response.set_content_length(0);
}

void handle_reorder_playlist_items(IDatabasePool& database, const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response))
    return;
  const auto playlist_id = parse_id(request, "id", response);
  if (!playlist_id)
    return;
  const auto payload = parse_object(request.body, response);
  if (!payload || !has_only_fields(*payload, {"music_ids"}) || !payload->contains("music_ids") ||
      !payload->at("music_ids").is_array()) {
    if (payload)
      response = json_error(400, "INVALID_REQUEST", "重排列表无效");
    return;
  }
  std::vector<int64_t> music_ids;
  music_ids.reserve(payload->at("music_ids").size());
  for (const auto& id : payload->at("music_ids")) {
    const auto music_id = strict_json_integer_value(id);
    if (!music_id || *music_id <= 0) {
      response = json_error(400, "INVALID_REQUEST", "重排列表无效");
      return;
    }
    music_ids.push_back(*music_id);
  }
  const auto result = database.reorder_playlist_items(*playlist_id, request.auth_user.user_id, music_ids);
  if (result.status != MutationStatus::OK) {
    response = mutation_error(result.status, result.detail);
    return;
  }
  set_json(response, 200, "OK", {{"message", "排序已更新"}});
}

void register_playlist_routes(IHttpServer& server, IDatabasePool& database) {
  server.get("/api/users/:id/playlists", [&database](const HttpRequest& request, HttpResponse& response) {
    handle_get_user_playlists(database, request, response);
  });
  server.post("/api/users/:id/playlists", [&database](const HttpRequest& request, HttpResponse& response) {
    handle_create_playlist(database, request, response);
  });
  server.put("/api/playlists/:id", [&database](const HttpRequest& request, HttpResponse& response) {
    handle_update_playlist(database, request, response);
  });
  server.del("/api/playlists/:id", [&database](const HttpRequest& request, HttpResponse& response) {
    handle_delete_playlist(database, request, response);
  });
  server.get("/api/playlists/:id/items", [&database](const HttpRequest& request, HttpResponse& response) {
    handle_get_playlist_items(database, request, response);
  });
  server.post("/api/playlists/:id/items", [&database](const HttpRequest& request, HttpResponse& response) {
    handle_add_playlist_item(database, request, response);
  });
  server.del("/api/playlists/:id/items/:music_id", [&database](const HttpRequest& request, HttpResponse& response) {
    handle_remove_playlist_item(database, request, response);
  });
  server.put("/api/playlists/:id/items/reorder", [&database](const HttpRequest& request, HttpResponse& response) {
    handle_reorder_playlist_items(database, request, response);
  });
}

} // namespace hps
