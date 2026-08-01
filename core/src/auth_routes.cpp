#include "auth_routes.h"

#include "auth_service.h"
#include "authorization.h"
#include "email_validation.h"
#include "http_request.h"
#include "http_response.h"
#include "i_http_server.h"
#include "idatabase_pool.h"
#include "password_validation.h"
#include "strict_json.h"

#include <charconv>
#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hps {

namespace {

HttpResponse auth_error(int status, const std::string& message, std::string code = {}) {
  HttpResponse response;
  if (status == 400) {
    response.set_status(status, "Bad Request");
  } else if (status == 401) {
    response.set_status(status, "Unauthorized");
  } else if (status == 403) {
    response.set_status(status, "Forbidden");
  } else if (status == 404) {
    response.set_status(status, "Not Found");
  } else if (status == 409) {
    response.set_status(status, "Conflict");
  } else {
    response.set_status(status, "Internal Server Error");
  }
  response.set_content_type("application/json");
  if (code.empty()) {
    if (status == 400) {
      code = "INVALID_REQUEST";
    } else if (status == 401) {
      code = "AUTH_REQUIRED";
    } else {
      code = "PERSISTENCE_ERROR";
    }
  }
  response.body = nlohmann::json{{"code", std::move(code)}, {"error", message}}.dump();
  response.set_content_length(response.body.size());
  return response;
}

bool require_authenticated(const HttpRequest& request, HttpResponse& response) {
  if (request.auth_status == TokenValidationStatus::STORAGE_ERROR) {
    response = auth_error(500, "认证存储暂时不可用", "PERSISTENCE_ERROR");
    return false;
  }
  if (!has_capability(request.auth_user, Capability::USE_AUTHENTICATED_FEATURES)) {
    response = auth_error(401, "需要登录");
    return false;
  }
  return true;
}

std::optional<int64_t> parse_profile_id(const HttpRequest& request) {
  const auto item = request.path_params.find("id");
  if (item == request.path_params.end() || item->second.empty())
    return std::nullopt;
  int64_t id = 0;
  const auto* begin = item->second.data();
  const auto* end = begin + item->second.size();
  const auto [position, error] = std::from_chars(begin, end, id);
  if (error != std::errc{} || position != end || id <= 0)
    return std::nullopt;
  return id;
}

void set_auth_user_response(HttpResponse& response, const User& user) {
  response.set_status(200, "OK");
  response.set_content_type("application/json");
  response.body = serialize_auth_user(user, make_effective_identity(user, std::chrono::system_clock::now())).dump();
  response.set_content_length(response.body.size());
}

void handle_register(IDatabasePool& db, IAuthService& auth, const HttpRequest& request, HttpResponse& response) {
  try {
    const auto body = parse_strict_json_object(request.body);
    if (!body) {
      response = auth_error(400, "请求格式错误");
      return;
    }
    if (has_forbidden_registration_fields(*body)) {
      response = auth_error(400, "注册请求不得设置角色、会员或管理员字段", "REGISTRATION_FIELD_FORBIDDEN");
      return;
    }
    if (!matches_strict_json_object(*body,
                                    {{"username", StrictJsonValueType::STRING, true},
                                     {"password", StrictJsonValueType::STRING, true},
                                     {"email", StrictJsonValueType::STRING, false}})) {
      response = auth_error(400, "请求格式错误");
      return;
    }
    const auto username = body->at("username").get<std::string>();
    const auto password = body->at("password").get<std::string>();
    const auto email = body->value("email", "");
    if (username.size() < 2 || !is_valid_user_password(password)) {
      response = auth_error(400, "用户名至少2字符，密码长度必须为6至128字节");
      return;
    }
    if (!email.empty() && !is_valid_email(email)) {
      response = auth_error(400, "邮箱格式或长度无效");
      return;
    }
    const auto username_owner = db.get_user_by_username_result(username);
    if (username_owner.status == LookupStatus::FOUND) {
      response = auth_error(409, "用户名已存在", "USERNAME_CONFLICT");
      return;
    }
    if (username_owner.status != LookupStatus::NOT_FOUND) {
      response = auth_error(500, "创建用户失败", "PERSISTENCE_ERROR");
      return;
    }
    if (!email.empty()) {
      const auto email_owner = db.get_user_by_email_result(email);
      if (email_owner.status == LookupStatus::FOUND) {
        response = auth_error(409, "邮箱已被使用", "EMAIL_CONFLICT");
        return;
      }
      if (email_owner.status != LookupStatus::NOT_FOUND) {
        response = auth_error(500, "创建用户失败", "PERSISTENCE_ERROR");
        return;
      }
    }
    User user;
    user.username = username;
    user.salt = generate_salt();
    user.password_hash = hash_password(password, user.salt);
    user.role = UserRole::NORMAL;
    user.email = email;
    const auto creation = db.create_user(user);
    if (creation.status == MutationStatus::CONFLICT && creation.detail) {
      response =
        auth_error(409, *creation.detail == "EMAIL_CONFLICT" ? "邮箱已被使用" : "用户名已存在", *creation.detail);
      return;
    }
    if (creation.status != MutationStatus::OK) {
      response = auth_error(500, "创建用户失败");
      return;
    }
    const auto auth_user = db.get_auth_user_result(username);
    if (auth_user.status != LookupStatus::FOUND || !auth_user.value) {
      response = auth_error(500, "创建用户失败");
      return;
    }
    const auto stored_user = db.get_user_result(auth_user.value->user_id);
    if (stored_user.status != LookupStatus::FOUND || !stored_user.value) {
      response = auth_error(500, "创建用户失败");
      return;
    }
    const auto token = auth.generate_token(*auth_user.value);
    if (token.empty()) {
      response = auth_error(500, "认证服务暂时不可用", "PERSISTENCE_ERROR");
      return;
    }
    const auto identity = make_effective_identity(*stored_user.value, std::chrono::system_clock::now());
    response.set_status(201, "Created");
    response.set_content_type("application/json");
    response.body = serialize_auth_response(token, *stored_user.value, identity).dump();
    response.set_content_length(response.body.size());
  } catch (...) {
    response = auth_error(500, "认证服务暂时不可用", "PERSISTENCE_ERROR");
  }
}

void handle_login(IDatabasePool& db, IAuthService& auth, const HttpRequest& request, HttpResponse& response) {
  try {
    const auto body = parse_strict_json_object(request.body,
                                               {{"username", StrictJsonValueType::STRING, true},
                                                {"password", StrictJsonValueType::STRING, true}});
    if (!body) {
      response = auth_error(400, "请求格式错误");
      return;
    }
    const auto username = body->at("username").get<std::string>();
    const auto password = body->at("password").get<std::string>();
    const auto authentication = auth.authenticate(username, password);
    if (authentication.status == AuthenticationStatus::STORAGE_ERROR) {
      response = auth_error(500, "认证服务暂时不可用", "PERSISTENCE_ERROR");
      return;
    }
    if (authentication.status != AuthenticationStatus::AUTHENTICATED || !authentication.user) {
      response = auth_error(401, "用户名或密码错误");
      return;
    }
    const auto stored_user = db.get_user_result(authentication.user->user_id);
    if (stored_user.status != LookupStatus::FOUND || !stored_user.value) {
      response = auth_error(500, "认证服务暂时不可用", "PERSISTENCE_ERROR");
      return;
    }
    const auto token = auth.generate_token(*authentication.user);
    if (token.empty()) {
      response = auth_error(500, "认证服务暂时不可用", "PERSISTENCE_ERROR");
      return;
    }
    const auto identity = make_effective_identity(*stored_user.value, std::chrono::system_clock::now());
    response.set_status(200, "OK");
    response.set_content_type("application/json");
    response.body = serialize_auth_response(token, *stored_user.value, identity).dump();
    response.set_content_length(response.body.size());
  } catch (...) {
    response = auth_error(500, "认证服务暂时不可用", "PERSISTENCE_ERROR");
  }
}

void handle_get_me(IDatabasePool& db, const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response)) {
    return;
  }
  try {
    const auto user = db.get_user_result(request.auth_user.user_id);
    if (user.status == LookupStatus::NOT_FOUND) {
      response = auth_error(401, "需要登录");
      return;
    }
    if (user.status != LookupStatus::FOUND || !user.value) {
      response = auth_error(500, "认证服务暂时不可用", "PERSISTENCE_ERROR");
      return;
    }
    const auto identity = make_effective_identity(*user.value, std::chrono::system_clock::now());
    response.set_status(200, "OK");
    response.set_content_type("application/json");
    response.body = serialize_auth_user(*user.value, identity).dump();
    response.set_content_length(response.body.size());
  } catch (...) {
    response = auth_error(500, "认证服务暂时不可用", "PERSISTENCE_ERROR");
  }
}

void handle_get_user(IDatabasePool& db, const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response))
    return;
  const auto target_id = parse_profile_id(request);
  if (!target_id) {
    response = auth_error(400, "用户 ID 无效", "INVALID_REQUEST");
    return;
  }
  if (*target_id != request.auth_user.user_id) {
    response = auth_error(403, "只能查看自己的信息", "PROFILE_OWNER_REQUIRED");
    return;
  }
  try {
    const auto user = db.get_user_result(*target_id);
    if (user.status == LookupStatus::NOT_FOUND) {
      response = auth_error(401, "需要登录");
      return;
    }
    if (user.status != LookupStatus::FOUND || !user.value) {
      response = auth_error(500, "用户资料暂时不可用", "PERSISTENCE_ERROR");
      return;
    }
    set_auth_user_response(response, *user.value);
  } catch (...) {
    response = auth_error(500, "用户资料暂时不可用", "PERSISTENCE_ERROR");
  }
}

void handle_put_user(IDatabasePool& db, const HttpRequest& request, HttpResponse& response) {
  if (!require_authenticated(request, response))
    return;
  const auto target_id = parse_profile_id(request);
  if (!target_id) {
    response = auth_error(400, "用户 ID 无效", "INVALID_REQUEST");
    return;
  }
  if (*target_id != request.auth_user.user_id) {
    response = auth_error(403, "只能修改自己的信息", "PROFILE_OWNER_REQUIRED");
    return;
  }
  const auto payload = parse_strict_json_object(request.body,
                                                {{"email", StrictJsonValueType::STRING, false},
                                                 {"password", StrictJsonValueType::STRING, false}});
  if (!payload || payload->empty()) {
    response = auth_error(400, "请求内容无效", "INVALID_REQUEST");
    return;
  }
  const auto email = payload->contains("email") ? std::optional(payload->at("email").get<std::string>()) : std::nullopt;
  const auto password = payload->contains("password") ? std::optional(payload->at("password").get<std::string>())
                                                      : std::nullopt;
  if ((email && !is_valid_email(*email)) || (password && !is_valid_user_password(*password))) {
    response = auth_error(400, "邮箱或密码无效", "INVALID_REQUEST");
    return;
  }
  try {
    User updated;
    updated.user_id = *target_id;
    if (email)
      updated.email = *email;
    if (password) {
      updated.salt = generate_salt();
      updated.password_hash = hash_password(*password, updated.salt);
    }
    const auto result = db.update_user(updated);
    if (result.status == MutationStatus::CONFLICT) {
      response = auth_error(409, "邮箱已被使用", "EMAIL_CONFLICT");
      return;
    }
    if (result.status == MutationStatus::NOT_FOUND) {
      response = auth_error(401, "需要登录");
      return;
    }
    if (result.status != MutationStatus::OK) {
      response = auth_error(500, "用户资料更新失败", "PERSISTENCE_ERROR");
      return;
    }
    const auto current = db.get_user_result(*target_id);
    if (current.status == LookupStatus::NOT_FOUND) {
      response = auth_error(401, "需要登录");
      return;
    }
    if (current.status != LookupStatus::FOUND || !current.value) {
      response = auth_error(500, "用户资料更新失败", "PERSISTENCE_ERROR");
      return;
    }
    set_auth_user_response(response, *current.value);
  } catch (...) {
    response = auth_error(500, "用户资料更新失败", "PERSISTENCE_ERROR");
  }
}

} // namespace

void register_auth_routes(IHttpServer& server, IDatabasePool& db, IAuthService& auth) {
  server.post("/api/auth/register", [&db, &auth](const HttpRequest& request, HttpResponse& response) {
    handle_register(db, auth, request, response);
  });
  server.post("/api/auth/login", [&db, &auth](const HttpRequest& request, HttpResponse& response) {
    handle_login(db, auth, request, response);
  });
  server.post("/api/auth/logout", [](const HttpRequest&, HttpResponse& response) {
    response.set_status(200, "OK");
    response.set_content_type("application/json");
    response.body = R"({"message":"已登出"})";
    response.set_content_length(response.body.size());
  });
  server.get("/api/auth/me",
             [&db](const HttpRequest& request, HttpResponse& response) { handle_get_me(db, request, response); });
  server.get("/api/users/:id",
             [&db](const HttpRequest& request, HttpResponse& response) { handle_get_user(db, request, response); });
  server.put("/api/users/:id",
             [&db](const HttpRequest& request, HttpResponse& response) { handle_put_user(db, request, response); });
}

} // namespace hps
