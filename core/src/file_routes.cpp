#include "file_routes.h"

#include "api_datetime.h"
#include "authorization.h"
#include "i_file_system.h"
#include "i_http_server.h"
#include "idatabase_pool.h"
#include "logger.h"
#include "pending_chunk_deletions.h"

#include <charconv>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hps {
namespace {

std::string_view http_status_text(int status) {
  switch (status) {
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 409:
      return "Conflict";
    case 422:
      return "Unprocessable Entity";
    default:
      return "Internal Server Error";
  }
}

HttpResponse json_error(int status, std::string_view code, std::string_view message) {
  HttpResponse response;
  response.set_status(status, http_status_text(status));
  response.set_content_type("application/json");
  response.body = nlohmann::json{{"code", code}, {"error", message}}.dump();
  response.set_content_length(response.body.size());
  return response;
}

bool authenticated(const HttpRequest& request, HttpResponse& response) {
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

std::optional<std::string> decode(std::string_view input) {
  std::string output;
  output.reserve(input.size());
  const auto hex = [](char value) -> int {
    if (value >= '0' && value <= '9')
      return value - '0';
    if (value >= 'a' && value <= 'f')
      return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
      return value - 'A' + 10;
    return -1;
  };
  for (std::size_t index = 0; index < input.size(); ++index) {
    if (input[index] == '+') {
      output.push_back(' ');
    } else if (input[index] == '%') {
      if (index + 2 >= input.size())
        return std::nullopt;
      const int high = hex(input[index + 1]);
      const int low = hex(input[index + 2]);
      if (high < 0 || low < 0)
        return std::nullopt;
      output.push_back(static_cast<char>((static_cast<unsigned>(high) << 4U) | static_cast<unsigned>(low)));
      index += 2;
    } else {
      output.push_back(input[index]);
    }
  }
  return output;
}

bool parse_nonnegative(std::string_view text, int& value) {
  const auto [position, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return !text.empty() && error == std::errc{} && position == text.data() + text.size() && value >= 0;
}

struct FileQuery {
  std::string name;
  std::string type;
  int offset{0};
  int limit{20};
};

std::optional<FileQuery> parse_query(std::string_view query) {
  FileQuery result;
  std::unordered_map<std::string, std::string> values;
  while (!query.empty()) {
    const auto separator = query.find('&');
    const auto item = query.substr(0, separator);
    if (item.empty())
      return std::nullopt;
    const auto equals = item.find('=');
    if (equals == std::string_view::npos)
      return std::nullopt;
    auto key = decode(item.substr(0, equals));
    auto value = decode(item.substr(equals + 1));
    if (!key || !value || (*key != "name" && *key != "type" && *key != "offset" && *key != "limit") ||
        values.contains(*key)) {
      return std::nullopt;
    }
    values.emplace(std::move(*key), std::move(*value));
    if (separator == std::string_view::npos)
      break;
    query.remove_prefix(separator + 1);
    if (query.empty())
      return std::nullopt;
  }
  if (values.contains("name"))
    result.name = values["name"];
  if (values.contains("type"))
    result.type = values["type"];
  if (!result.type.empty() && result.type != "audio" && result.type != "image" && result.type != "video" &&
      result.type != "other")
    return std::nullopt;
  if (values.contains("offset") && !parse_nonnegative(values["offset"], result.offset))
    return std::nullopt;
  if (values.contains("limit") &&
      (!parse_nonnegative(values["limit"], result.limit) || result.limit < 1 || result.limit > 100))
    return std::nullopt;
  return result;
}

std::optional<int64_t> parse_id(const HttpRequest& request) {
  const auto item = request.path_params.find("id");
  if (item == request.path_params.end())
    return std::nullopt;
  int64_t id = 0;
  const auto [position, error] = std::from_chars(item->second.data(), item->second.data() + item->second.size(), id);
  if (error != std::errc{} || position != item->second.data() + item->second.size() || id <= 0)
    return std::nullopt;
  return id;
}

std::optional<nlohmann::json> file_json(const FileRecord& record, bool can_delete) {
  const auto created_at = format_api_datetime(record.created_at);
  if (!created_at) {
    return std::nullopt;
  }
  return nlohmann::json{{"file_id", record.file_id},
                        {"file_name", record.file_name},
                        {"file_hash", record.file_hash},
                        {"file_size", record.file_size},
                        {"content_type", record.content_type},
                        {"uploaded_by", record.uploaded_by},
                        {"can_delete", can_delete},
                        {"created_at", *created_at}};
}

void handle_list_files(IDatabasePool& database, const HttpRequest& request, HttpResponse& response) {
  if (!authenticated(request, response))
    return;
  const auto query = parse_query(request.query_string);
  if (!query) {
    response = json_error(400, "INVALID_REQUEST", "文件查询参数无效");
    return;
  }
  const auto page = database.list_files(query->name, query->type, query->offset, query->limit);
  if (page.status == LookupStatus::INVALID_DATA) {
    response = json_error(422, "INVALID_STATE", "文件列表数据无效");
    return;
  }
  if (page.status != LookupStatus::FOUND || !page.value) {
    response = json_error(500, "PERSISTENCE_ERROR", "文件列表暂时不可用");
    return;
  }
  nlohmann::json items = nlohmann::json::array();
  const bool delete_any = has_capability(request.auth_user, Capability::DELETE_ANY_FILE);
  for (const auto& record : page.value->items) {
    auto item = file_json(record, delete_any || record.uploaded_by == request.auth_user.user_id);
    if (!item) {
      response = json_error(422, "INVALID_STATE", "文件列表数据无效");
      return;
    }
    items.push_back(std::move(*item));
  }
  response.set_status(200, "OK");
  response.set_content_type("application/json");
  response.body = nlohmann::json{{"items", std::move(items)},
                                 {"total", page.value->total},
                                 {"offset", page.value->offset},
                                 {"limit", page.value->limit}}
                    .dump();
  response.set_content_length(response.body.size());
}

void handle_get_file(IDatabasePool& database, const HttpRequest& request, HttpResponse& response) {
  if (!authenticated(request, response))
    return;
  const auto id = parse_id(request);
  if (!id) {
    response = json_error(400, "INVALID_REQUEST", "文件 ID 无效");
    return;
  }
  try {
    const auto record = database.get_file_record_result(*id);
    if (record.status == LookupStatus::NOT_FOUND) {
      response = json_error(404, "FILE_NOT_FOUND", "文件不存在");
      return;
    }
    if (record.status == LookupStatus::INVALID_DATA) {
      response = json_error(422, "INVALID_STATE", "文件详情数据无效");
      return;
    }
    if (record.status != LookupStatus::FOUND || !record.value) {
      response = json_error(500, "PERSISTENCE_ERROR", "文件详情暂时不可用");
      return;
    }
    const bool can_delete = record.value->uploaded_by == request.auth_user.user_id ||
                            has_capability(request.auth_user, Capability::DELETE_ANY_FILE);
    const auto body = file_json(*record.value, can_delete);
    if (!body) {
      response = json_error(422, "INVALID_STATE", "文件详情数据无效");
      return;
    }
    response.set_status(200, "OK");
    response.set_content_type("application/json");
    response.body = body->dump();
    response.set_content_length(response.body.size());
  } catch (...) {
    response = json_error(500, "PERSISTENCE_ERROR", "文件详情暂时不可用");
  }
}

void handle_delete_file(IDatabasePool& database,
                        IFileSystem& file_system,
                        ChunkLifecycleCoordinator& coordinator,
                        const HttpRequest& request,
                        HttpResponse& response) {
  if (!authenticated(request, response))
    return;
  const auto id = parse_id(request);
  if (!id) {
    response = json_error(400, "INVALID_REQUEST", "文件 ID 无效");
    return;
  }
  auto cleanup_guard = coordinator.acquire_cleanup_guard();
  const auto result = database.delete_file_owned(cleanup_guard.permit(),
                                                 *id,
                                                 request.auth_user.user_id,
                                                 has_capability(request.auth_user, Capability::DELETE_ANY_FILE));
  if (result.status == MutationStatus::NOT_FOUND) {
    response = json_error(404, "FILE_NOT_FOUND", "文件不存在");
    return;
  }
  if (result.status == MutationStatus::OWNER_REQUIRED) {
    response = json_error(403, "FILE_DELETE_FORBIDDEN", "只能删除自己上传的文件");
    return;
  }
  if (result.status == MutationStatus::CONFLICT) {
    response = json_error(409, "FILE_MUSIC_CHANGED", "文件关联状态已变化，请重试");
    return;
  }
  if (result.status == MutationStatus::INVALID_STATE) {
    response = json_error(422, "INVALID_STATE", "文件状态无效");
    return;
  }
  if (result.status != MutationStatus::OK || !result.value) {
    response = json_error(500, "PERSISTENCE_ERROR", "文件删除失败");
    return;
  }
  const auto cleanup = run_pending_chunk_deletions_guarded(database, file_system, cleanup_guard.permit(), 32);
  const bool cleanup_deferred = cleanup.status != MutationStatus::OK;
  if (cleanup_deferred) {
    Logger::_warn("pending chunk cleanup deferred after committed file deletion");
  }
  response.set_status(200, "OK");
  response.set_content_type("application/json");
  response.body = nlohmann::json{{"message", "已删除"},
                                 {"file_id", result.value->file_id},
                                 {"queued_chunk_count", result.value->queued_chunk_count},
                                 {"cleanup_deferred", cleanup_deferred}}
                    .dump();
  response.set_content_length(response.body.size());
}

} // namespace

void register_file_routes(IHttpServer& server,
                          IDatabasePool& database,
                          IFileSystem& file_system,
                          ChunkLifecycleCoordinator& coordinator) {
  server.get("/api/files", [&database](const HttpRequest& request, HttpResponse& response) {
    handle_list_files(database, request, response);
  });

  server.get("/api/files/:id", [&database](const HttpRequest& request, HttpResponse& response) {
    handle_get_file(database, request, response);
  });

  server.del("/api/files/:id",
             [&database, &file_system, &coordinator](const HttpRequest& request, HttpResponse& response) {
               handle_delete_file(database, file_system, coordinator, request, response);
             });
}

} // namespace hps
