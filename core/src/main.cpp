#include "auth_middleware.h"
#include "auth_service.h"
#include "boost_mysql_connection.h"
#include "database_pool.h"
#include "file_system.h"
#include "http_server.h"
#include "i_http_server.h"
#include "idatabase_pool.h"
#include "logappender.h"
#include "logformatter.h"
#include "logger.h"
#include "main_functions.h"
#include "range_parser.h"
#include "ssl_context.h"
#include "stream_download_utils.h"
#include "upload_policy.h"
#include "ws_connection.h"

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hps {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::chrono::steady_clock::time_point g_start_time;

namespace {

HttpResponse auth_error(int code, const std::string& msg) {
  HttpResponse resp;
  resp.set_status(code, code == 401 ? "Unauthorized" : "Forbidden");
  resp.set_content_type("application/json");
  resp.body = R"({"error":")" + msg + R"("})";
  resp.set_content_length(resp.body.size());
  return resp;
}

bool check_auth(const HttpRequest& req, HttpResponse& resp, UserRole min_role) {
  if (req.auth_user.role == UserRole::GUEST) {
    resp = auth_error(401, "需要登录");
    return false;
  }
  if (req.auth_user.role < min_role) {
    resp = auth_error(403, "权限不足");
    return false;
  }
  return true;
}

std::string stem(const std::string& filename) {
  auto dot = filename.rfind('.');
  if (dot == std::string::npos) {
    return filename;
  }
  return filename.substr(0, dot);
}

} // anonymous namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void register_routes(HttpServer& server,
                     DatabasePool& db,
                     FileSystem& fs,
                     IAuthService& auth,
                     const ServerConfig& cfg) {
  server.post("/api/auth/register", [&db, &auth](const HttpRequest& req, HttpResponse& resp) {
    try {
      auto json = nlohmann::json::parse(req.body);
      auto username = json["username"].get<std::string>();
      auto password = json["password"].get<std::string>();
      auto email = json.value("email", "");
      if (username.size() < 2 || password.size() < 6) {
        resp = auth_error(400, "用户名至少2字符，密码至少6字符");
        return;
      }
      if (db.username_exists(username)) {
        resp.set_status(400, "Bad Request");
        resp.set_content_type("application/json");
        resp.body = R"({"error":"用户名已存在"})";
        resp.set_content_length(resp.body.size());
        return;
      }
      auto salt = hps::generate_salt();
      auto hashed = hps::hash_password(password, salt);
      User user;
      user.username = username;
      user.password_hash = hashed;
      user.salt = salt;
      user.role = UserRole::NORMAL;
      user.email = email;
      if (!db.create_user(user)) {
        resp = auth_error(500, "创建用户失败");
        return;
      }
      auto auth_user = db.get_auth_user(username);
      if (!auth_user) {
        resp = auth_error(500, "创建用户失败");
        return;
      }
      auto token = auth.generate_token(*auth_user);
      resp.set_status(201, "Created");
      resp.set_content_type("application/json");
      resp.body = R"({"token":")" + token + R"(","user_id":)" + std::to_string(auth_user->user_id) +
                  R"(,"username":")" + username + R"(","role":)" + std::to_string(static_cast<int>(auth_user->role)) +
                  R"(})";
      resp.set_content_length(resp.body.size());
    } catch (...) {
      resp = auth_error(400, "请求格式错误");
    }
  });

  server.post("/api/auth/login", [&auth](const HttpRequest& req, HttpResponse& resp) {
    try {
      auto json = nlohmann::json::parse(req.body);
      auto username = json["username"].get<std::string>();
      auto password = json["password"].get<std::string>();
      auto user = auth.authenticate(username, password);
      if (!user) {
        resp = auth_error(401, "用户名或密码错误");
        return;
      }
      auto token = auth.generate_token(*user);
      resp.set_status(200, "OK");
      resp.set_content_type("application/json");
      resp.body = R"({"token":")" + token + R"(","user_id":)" + std::to_string(user->user_id) + R"(,"role":)" +
                  std::to_string(static_cast<int>(user->role)) + R"(})";
      resp.set_content_length(resp.body.size());
    } catch (...) {
      resp = auth_error(400, "请求格式错误");
    }
  });

  server.post("/api/auth/logout", [](const HttpRequest&, HttpResponse& resp) {
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    resp.body = R"({"message":"已登出"})";
    resp.set_content_length(resp.body.size());
  });

  server.get("/api/auth/me", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (req.auth_user.role == UserRole::GUEST) {
      resp = auth_error(401, "未登录");
      return;
    }
    auto user = db.get_user(req.auth_user.user_id);
    if (!user) {
      resp = auth_error(404, "用户不存在");
      return;
    }
    std::string role_str;
    if (user->role == UserRole::VIP)
      role_str = "VIP";
    else if (user->role == UserRole::NORMAL)
      role_str = "NORMAL";
    else
      role_str = "GUEST";
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    resp.body = R"({"user_id":)" + std::to_string(user->user_id) + R"(,"username":")" + user->username +
                R"(","email":")" + user->email + R"(","role":")" + role_str + R"(","created_at":")" + user->created_at +
                R"("})";
    resp.set_content_length(resp.body.size());
  });

  server.get("/api/health", [](const HttpRequest&, HttpResponse& resp) {
    auto uptime =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_start_time).count();
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    resp.body = R"({"status":"ok","uptime":)" + std::to_string(uptime) + R"(})";
    resp.set_content_length(resp.body.size());
  });

  server.get("/api/files", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::NORMAL)) {
      return;
    }
    std::string name_pattern;
    std::string type_filter;
    int offset = 0;
    int limit = 20;
    auto q = req.query_string;
    auto npos = q.find("name=");
    if (npos != std::string::npos) {
      auto end = q.find('&', npos);
      name_pattern = q.substr(npos + 5, end == std::string::npos ? end : end - npos - 5);
    }
    auto tpos = q.find("type=");
    if (tpos != std::string::npos) {
      auto end = q.find('&', tpos);
      type_filter = q.substr(tpos + 5, end == std::string::npos ? end : end - tpos - 5);
    }
    auto opos = q.find("offset=");
    if (opos != std::string::npos) {
      auto end = q.find('&', opos);
      offset = std::stoi(q.substr(opos + 7, end == std::string::npos ? end : end - opos - 7));
    }
    auto lpos = q.find("limit=");
    if (lpos != std::string::npos) {
      auto end = q.find('&', lpos);
      limit = std::stoi(q.substr(lpos + 6, end == std::string::npos ? end : end - lpos - 6));
    }
    int total = 0;
    auto records = db.search_files_ext(name_pattern, type_filter, offset, limit, total);
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    nlohmann::json items = nlohmann::json::array();
    std::ranges::transform(records, std::back_inserter(items), [](const FileRecord& record) {
      return nlohmann::json{{"file_id", record.file_id},
                            {"file_name", record.file_name},
                            {"file_hash", record.file_hash},
                            {"file_size", record.file_size},
                            {"content_type", record.content_type},
                            {"music_id", record.music_id}};
    });
    resp.body =
      nlohmann::json{{"items", std::move(items)}, {"total", total}, {"offset", offset}, {"limit", limit}}.dump();
    resp.set_content_length(resp.body.size());
  });

  server.get("/api/files/:id", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::NORMAL)) {
      return;
    }
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp = auth_error(400, "missing id");
      return;
    }
    auto record = db.get_file_record(std::stoll(it->second));
    if (!record) {
      resp.set_status(404, "Not Found");
      resp.set_content_type("application/json");
      resp.body = R"({"error":"file not found"})";
      resp.set_content_length(resp.body.size());
      return;
    }
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    resp.body = nlohmann::json{{"file_id", record->file_id},
                               {"file_name", record->file_name},
                               {"file_hash", record->file_hash},
                               {"file_size", record->file_size},
                               {"content_type", record->content_type}}
                  .dump();
    resp.set_content_length(resp.body.size());
  });

  server.get("/api/files/:id/download", [&db, &fs](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::NORMAL)) {
      return;
    }
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp = auth_error(400, "missing id");
      return;
    }
    auto record = db.get_file_record(std::stoll(it->second));
    if (!record) {
      resp.set_status(404, "Not Found");
      resp.set_content_type("application/json");
      resp.body = R"({"error":"file not found"})";
      resp.set_content_length(resp.body.size());
      return;
    }
    auto chunks = db.get_file_chunks(record->file_hash);
    if (chunks.empty()) {
      resp.set_status(500, "Internal Server Error");
      resp.set_content_type("application/json");
      resp.body = R"({"error":"no chunks"})";
      resp.set_content_length(resp.body.size());
      return;
    }
    std::string body_data;
    body_data.reserve(record->file_size);
    for (const auto& c : chunks) {
      auto data = fs.read_file("chunks/" + c.chunk_hash);
      if (!data) {
        resp.set_status(500, "Internal Server Error");
        resp.set_content_type("application/json");
        resp.body = R"({"error":"chunk read failed"})";
        resp.set_content_length(resp.body.size());
        return;
      }
      body_data.append(data->data(), data->size());
    }
    resp.set_status(200, "OK");
    resp.set_content_type(record->content_type.empty() ? "application/octet-stream" : record->content_type);
    resp.body = std::move(body_data);
    resp.set_content_length(resp.body.size());
    resp.set_header("Content-Disposition", build_attachment_content_disposition(record->file_name));
  });

  server.get("/api/files/:id/stream", [&db, &fs](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::NORMAL)) {
      return;
    }
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp = auth_error(400, "missing id");
      return;
    }
    auto record = db.get_file_record(std::stoll(it->second));
    if (!record) {
      resp = auth_error(404, "file not found");
      return;
    }
    auto chunks = db.get_file_chunks(record->file_hash);
    if (chunks.empty()) {
      resp = auth_error(500, "no chunks");
      return;
    }
    auto content_type = record->content_type;
    if (content_type == "application/octet-stream") {
      content_type = std::string(audio_content_type(record->file_name).value_or("application/octet-stream"));
    }
    resp.set_header("Accept-Ranges", "bytes");
    resp.set_content_type(content_type);
    auto range_it = req.headers.find("Range");
    if (range_it != req.headers.end()) {
      auto range_req = parse_range_header(range_it->second, record->file_size);
      if (!range_req.valid || !range_req.satisfiable || range_req.ranges.empty()) {
        build_416_response(resp, record->file_size);
        return;
      }
      build_206_headers(resp, range_req, record->file_size);
      resp.set_content_type(content_type);
      std::string body_data;
      if (!read_stream_range(fs, chunks, range_req.ranges[0].start, range_req.ranges[0].end, body_data)) {
        resp = auth_error(500, "chunk read failed");
        return;
      }
      resp.body = std::move(body_data);
      resp.set_content_length(range_req.ranges[0].end - range_req.ranges[0].start);
    } else {
      resp.set_status(200, "OK");
      std::string body_data;
      if (!read_stream_file(fs, chunks, record->file_size, body_data)) {
        resp = auth_error(500, "chunk read failed");
        return;
      }
      resp.body = std::move(body_data);
      resp.set_content_length(record->file_size);
    }
  });

  // N4 — 文件搜索
  server.get("/api/files/search", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::NORMAL)) {
      return;
    }
    std::string q;
    int offset = 0;
    int limit = 20;
    auto qs = req.query_string;
    auto qpos = qs.find("q=");
    if (qpos != std::string::npos) {
      auto end = qs.find('&', qpos);
      q = qs.substr(qpos + 2, end == std::string::npos ? end : end - qpos - 2);
    }
    auto opos = qs.find("offset=");
    if (opos != std::string::npos) {
      auto end = qs.find('&', opos);
      offset = std::stoi(qs.substr(opos + 7, end == std::string::npos ? end : end - opos - 7));
    }
    auto lpos = qs.find("limit=");
    if (lpos != std::string::npos) {
      auto end = qs.find('&', lpos);
      limit = std::stoi(qs.substr(lpos + 6, end == std::string::npos ? end : end - lpos - 6));
    }
    int total = 0;
    auto records = db.search_files_ext(q, "", offset, limit, total);
    if (!records.empty() && !q.empty()) {
      for (auto& r : records) {
        auto meta = db.get_music_by_file_id(r.file_id);
        if (meta) {
          r.music_id = meta->music_id;
        }
      }
    }
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    nlohmann::json items = nlohmann::json::array();
    std::ranges::transform(records, std::back_inserter(items), [](const FileRecord& record) {
      return nlohmann::json{{"file_id", record.file_id},
                            {"file_name", record.file_name},
                            {"file_hash", record.file_hash},
                            {"file_size", record.file_size},
                            {"content_type", record.content_type}};
    });
    resp.body =
      nlohmann::json{{"items", std::move(items)}, {"total", total}, {"offset", offset}, {"limit", limit}}.dump();
    resp.set_content_length(resp.body.size());
  });

  // N10 — 文件删除（VIP only）
  server.del("/api/files/:id", [&db, &fs](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::VIP)) {
      return;
    }
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp = auth_error(400, "missing id");
      return;
    }
    auto record = db.get_file_record(std::stoll(it->second));
    if (!record) {
      resp.set_status(404, "Not Found");
      resp.set_content_type("application/json");
      resp.body = R"({"error":"file not found"})";
      resp.set_content_length(resp.body.size());
      return;
    }
    auto chunks = db.get_file_chunks(record->file_hash);
    for (const auto& c : chunks) {
      fs.delete_file("chunks/" + c.chunk_hash);
    }
    if (!db.delete_file_record(record->file_id)) {
      resp.set_status(500, "Internal Server Error");
      resp.set_content_type("application/json");
      resp.body = R"({"error":"删除失败"})";
      resp.set_content_length(resp.body.size());
      return;
    }
    if (record->music_id > 0) {
      auto music = db.get_music_meta(record->music_id);
      if (music) {
        db.delete_music_meta(record->music_id);
      }
    }
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    resp.body = R"({"message":"已删除"})";
    resp.set_content_length(resp.body.size());
  });

  // N2 — 用户信息更新
  server.put("/api/users/:id", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (req.auth_user.role == UserRole::GUEST) {
      resp = auth_error(401, "需要登录");
      return;
    }
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp = auth_error(400, "missing id");
      return;
    }
    auto target_id = std::stoll(it->second);
    if (req.auth_user.user_id != target_id) {
      resp = auth_error(403, "只能修改自己的信息");
      return;
    }
    try {
      auto json = nlohmann::json::parse(req.body);
      auto user = db.get_user(target_id);
      if (!user) {
        resp = auth_error(404, "用户不存在");
        return;
      }
      if (json.contains("email")) {
        user->email = json["email"].get<std::string>();
      }
      if (json.contains("password") && !json["password"].get<std::string>().empty()) {
        auto new_pw = json["password"].get<std::string>();
        user->salt = generate_salt();
        user->password_hash = hash_password(new_pw, user->salt);
      }
      if (!db.update_user(*user)) {
        resp = auth_error(500, "更新失败");
        return;
      }
      resp.set_status(200, "OK");
      resp.set_content_type("application/json");
      resp.body = R"({"message":"已更新"})";
      resp.set_content_length(resp.body.size());
    } catch (...) {
      resp = auth_error(400, "请求格式错误");
    }
  });

  // M1 — 音乐库搜索
  server.get("/api/music/library", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::NORMAL)) {
      return;
    }
    std::string search;
    int offset = 0;
    int limit = 20;
    auto qs = req.query_string;
    auto spos = qs.find("search=");
    if (spos != std::string::npos) {
      auto end = qs.find('&', spos);
      search = qs.substr(spos + 7, end == std::string::npos ? end : end - spos - 7);
    }
    auto opos = qs.find("offset=");
    if (opos != std::string::npos) {
      auto end = qs.find('&', opos);
      offset = std::stoi(qs.substr(opos + 7, end == std::string::npos ? end : end - opos - 7));
    }
    auto lpos = qs.find("limit=");
    if (lpos != std::string::npos) {
      auto end = qs.find('&', lpos);
      limit = std::stoi(qs.substr(lpos + 6, end == std::string::npos ? end : end - lpos - 6));
    }
    int total = 0;
    auto items = db.list_music_library(search, offset, limit, total);
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    std::string body = R"({"items":[)";
    for (size_t i = 0; i < items.size(); ++i) {
      if (i > 0)
        body += ",";
      body += R"({"music_id":)" + std::to_string(items[i].music_id) + R"(,"title":")" + items[i].title +
              R"(","artist":")" + items[i].artist + R"(","album":")" + items[i].album + R"(","genre":")" +
              items[i].genre + R"(","duration_sec":)" + std::to_string(items[i].duration_sec) + R"(})";
    }
    body += R"(],"total":)" + std::to_string(total) + R"(,"offset":)" + std::to_string(offset) + R"(,"limit":)" +
            std::to_string(limit) + R"(})";
    resp.body = body;
    resp.set_content_length(resp.body.size());
  });

  // M2 — 音乐详情
  server.get("/api/music/library/:id", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::NORMAL)) {
      return;
    }
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp = auth_error(400, "missing id");
      return;
    }
    auto music = db.get_music_meta(std::stoll(it->second));
    if (!music) {
      resp.set_status(404, "Not Found");
      resp.set_content_type("application/json");
      resp.body = R"({"error":"music not found"})";
      resp.set_content_length(resp.body.size());
      return;
    }
    int total = 0;
    auto files = db.search_files_ext("", "audio", 0, 1000, total);
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    std::string body = R"({"music_id":)" + std::to_string(music->music_id) + R"(,"title":")" + music->title +
                       R"(","artist":")" + music->artist + R"(","album":")" + music->album + R"(","genre":")" +
                       music->genre + R"(","duration_sec":)" + std::to_string(music->duration_sec) + R"(,"files":[)";
    bool first = true;
    for (const auto& f : files) {
      if (f.music_id != music->music_id)
        continue;
      if (!first)
        body += ",";
      first = false;
      body += R"({"file_id":)" + std::to_string(f.file_id) + R"(,"file_hash":")" + f.file_hash + R"(","file_size":)" +
              std::to_string(f.file_size) + R"(,"content_type":")" + f.content_type + R"("})";
    }
    body += R"(]})";
    resp.body = body;
    resp.set_content_length(resp.body.size());
  });

  // M3 — 用户歌单列表
  server.get("/api/users/:id/playlists", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (req.auth_user.role == UserRole::GUEST) {
      resp = auth_error(401, "需要登录");
      return;
    }
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp = auth_error(400, "missing id");
      return;
    }
    auto playlists = db.get_user_playlists(std::stoll(it->second));
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    std::string body = R"({"playlists":[)";
    for (size_t i = 0; i < playlists.size(); ++i) {
      if (i > 0)
        body += ",";
      body += R"({"id":)" + std::to_string(playlists[i].playlist_id) + R"(,"name":")" + playlists[i].name +
              R"(","description":")" + playlists[i].description + R"(","item_count":)" +
              std::to_string(playlists[i].item_count) + R"(,"created_at":")" + playlists[i].created_at + R"("})";
    }
    body += R"(]})";
    resp.body = body;
    resp.set_content_length(resp.body.size());
  });

  // M4 — 创建歌单
  server.post("/api/users/:id/playlists", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (req.auth_user.role == UserRole::GUEST) {
      resp = auth_error(401, "需要登录");
      return;
    }
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp = auth_error(400, "missing id");
      return;
    }
    auto user_id = std::stoll(it->second);
    if (req.auth_user.user_id != user_id) {
      resp = auth_error(403, "只能创建自己的歌单");
      return;
    }
    try {
      auto json = nlohmann::json::parse(req.body);
      Playlist pl;
      pl.user_id = user_id;
      pl.name = json.value("name", "默认歌单");
      pl.description = json.value("description", "");
      auto id = db.create_playlist(pl);
      if (id <= 0) {
        resp = auth_error(500, "创建失败");
        return;
      }
      resp.set_status(201, "Created");
      resp.set_content_type("application/json");
      resp.body = R"({"playlist_id":)" + std::to_string(id) + R"(,"name":")" + pl.name + R"("})";
      resp.set_content_length(resp.body.size());
    } catch (...) {
      resp = auth_error(400, "请求格式错误");
    }
  });

  // M5 — 歌单项列表
  server.get("/api/playlists/:id/items", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::NORMAL)) {
      return;
    }
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp = auth_error(400, "missing id");
      return;
    }
    auto playlist_id = std::stoll(it->second);
    auto items = db.get_playlist_items(playlist_id);
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    std::string body = R"({"playlist_id":)" + std::to_string(playlist_id) + R"(,"items":[)";
    for (size_t i = 0; i < items.size(); ++i) {
      if (i > 0)
        body += ",";
      body += R"({"id":)" + std::to_string(items[i].id) + R"(,"music_id":)" + std::to_string(items[i].music_id) +
              R"(,"title":")" + items[i].title + R"(","artist":")" + items[i].artist + R"(","file_hash":")" +
              items[i].file_hash + R"(","sort_order":)" + std::to_string(items[i].sort_order) + R"(,"added_at":")" +
              items[i].added_at + R"("})";
    }
    body += R"(]})";
    resp.body = body;
    resp.set_content_length(resp.body.size());
  });

  // M6 — 添加歌单项
  server.post("/api/playlists/:id/items", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::NORMAL)) {
      return;
    }
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp = auth_error(400, "missing id");
      return;
    }
    try {
      auto json = nlohmann::json::parse(req.body);
      auto music_id = json["music_id"].get<int64_t>();
      auto ok = db.add_playlist_item(std::stoll(it->second), music_id);
      if (!ok) {
        resp.set_status(409, "Conflict");
        resp.set_content_type("application/json");
        resp.body = R"({"error":"歌曲已在歌单中"})";
        resp.set_content_length(resp.body.size());
        return;
      }
      resp.set_status(201, "Created");
      resp.set_content_type("application/json");
      resp.body = R"({"message":"已添加"})";
      resp.set_content_length(resp.body.size());
    } catch (...) {
      resp = auth_error(400, "请求格式错误");
    }
  });

  // M7 — 移除歌单项
  server.del("/api/playlists/:id/items/:music_id", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::NORMAL)) {
      return;
    }
    auto pit = req.path_params.find("id");
    auto mit = req.path_params.find("music_id");
    if (pit == req.path_params.end() || mit == req.path_params.end()) {
      resp = auth_error(400, "missing params");
      return;
    }
    auto ok = db.remove_playlist_item(std::stoll(pit->second), std::stoll(mit->second));
    if (!ok) {
      resp = auth_error(404, "未找到该歌曲");
      return;
    }
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    resp.body = R"({"message":"已移除"})";
    resp.set_content_length(resp.body.size());
  });

  // M8 — 重新排序
  server.put("/api/playlists/:id/items/reorder", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::NORMAL)) {
      return;
    }
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp = auth_error(400, "missing id");
      return;
    }
    try {
      auto json = nlohmann::json::parse(req.body);
      auto music_ids = json["music_ids"].get<std::vector<int64_t>>();
      auto ok = db.reorder_playlist_items(std::stoll(it->second), music_ids);
      if (!ok) {
        resp = auth_error(500, "排序失败");
        return;
      }
      resp.set_status(200, "OK");
      resp.set_content_type("application/json");
      resp.body = R"({"message":"排序已更新"})";
      resp.set_content_length(resp.body.size());
    } catch (...) {
      resp = auth_error(400, "请求格式错误");
    }
  });

  auto upload_setup = [&fs](const HttpRequest&, UploadStreamContext& ctx, HttpParser&) -> void {
    ctx.store_chunk_data = [&fs](std::string_view data, const std::string& chunk_hash) -> bool {
      std::vector<char> buf(data.begin(), data.end());
      return fs.store_file("chunks/" + chunk_hash, buf);
    };
    const auto file_name = ctx.file_name;
    ctx.set_initial_chunk_probe(kAudioSignatureProbeSize,
                                [file_name](std::string_view prefix) -> std::optional<HttpResponse> {
                                  const auto validation =
                                    validate_audio_signature(file_name, AudioSignaturePrefix{prefix});
                                  if (validation.accepted) {
                                    return std::nullopt;
                                  }
                                  return make_upload_validation_response(validation);
                                });
  };

  auto upload_preflight = [&cfg](const HttpRequest& req,
                                 const UploadStreamContext& ctx) -> std::optional<HttpResponse> {
    const auto validation = validate_audio_upload(ctx.file_name, ctx.content_length, req.auth_user.role, cfg);
    if (validation.accepted) {
      return std::nullopt;
    }
    return make_upload_validation_response(validation);
  };

  server.upload(
    "/api/files/upload",
    [&db, &cfg](const HttpRequest& req, UploadStreamContext& ctx, HttpResponse& resp) {
      if (!check_auth(req, resp, UserRole::NORMAL)) {
        return;
      }
      const auto validation = validate_audio_upload(ctx.file_name, ctx.content_length, req.auth_user.role, cfg);
      if (!validation.accepted) {
        resp = make_upload_validation_response(validation);
        return;
      }

      auto* md_ctx = static_cast<EVP_MD_CTX*>(ctx.hash_ctx);
      if (md_ctx == nullptr) {
        resp = auth_error(500, "文件哈希初始化失败");
        return;
      }
      std::array<unsigned char, EVP_MAX_MD_SIZE> final_hash{};
      unsigned int hash_len = 0;
      if (EVP_DigestFinal_ex(md_ctx, final_hash.data(), &hash_len) != 1) {
        ctx.reset_hash_context();
        resp = auth_error(500, "文件哈希计算失败");
        return;
      }
      ctx.reset_hash_context();

      auto overall_hash = FileSystem::sha256_hex(reinterpret_cast<const char*>(final_hash.data()), hash_len);
      ctx.file_hash = overall_hash;

      for (auto& c : ctx.chunks) {
        c.file_hash = overall_hash;
      }

      auto existing = db.get_file_record_by_hash(overall_hash);
      if (existing) {
        resp.set_status(200, "OK");
        resp.set_content_type("application/json");
        resp.body = nlohmann::json{{"file_id", existing->file_id},
                                   {"file_name", existing->file_name},
                                   {"file_hash", overall_hash},
                                   {"size", ctx.total_size},
                                   {"exists", true}}
                      .dump();
        resp.set_content_length(resp.body.size());
        return;
      }

      FileRecord record;
      record.file_name = ctx.file_name;
      record.file_hash = overall_hash;
      record.file_size = ctx.total_size;
      record.content_type = validation.content_type;
      record.chunk_size = 2097152;
      record.uploaded_by = req.auth_user.user_id;
      auto file_id = db.store_file_record(record);
      if (!file_id) {
        resp = auth_error(500, "保存文件记录失败");
        return;
      }
      record.file_id = *file_id;
      if (!db.store_file_chunks(ctx.chunks)) {
        resp = auth_error(500, "保存文件分片记录失败");
        return;
      }

      if (record.content_type.starts_with("audio/")) {
        MusicMeta meta;
        meta.title = stem(ctx.file_name);
        meta.artist = "";
        meta.album = "";
        meta.genre = "";
        meta.duration_sec = 0;
        auto music_id = db.create_music_meta(meta);
        if (music_id <= 0) {
          resp = auth_error(500, "保存音乐信息失败");
          return;
        }
        record.music_id = music_id;
        if (!db.update_file_record(record)) {
          resp = auth_error(500, "关联音乐信息失败");
          return;
        }
      }

      resp.set_status(201, "Created");
      resp.set_content_type("application/json");
      resp.body = nlohmann::json{{"file_id", record.file_id},
                                 {"file_name", record.file_name},
                                 {"file_hash", overall_hash},
                                 {"size", ctx.total_size},
                                 {"chunks", ctx.chunks.size()}}
                    .dump();
      resp.set_content_length(resp.body.size());
    },
    upload_setup,
    upload_preflight);

  server.get("/api/files/by-hash/:hash/download", [&db, &fs](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, UserRole::NORMAL)) {
      return;
    }
    auto it = req.path_params.find("hash");
    if (it == req.path_params.end()) {
      resp = auth_error(400, "missing hash");
      return;
    }
    auto record = db.get_file_record_by_hash(it->second);
    if (!record) {
      resp.set_status(404, "Not Found");
      resp.set_content_type("application/json");
      resp.body = R"({"error":"file not found"})";
      resp.set_content_length(resp.body.size());
      return;
    }
    auto chunks = db.get_file_chunks(record->file_hash);
    if (chunks.empty()) {
      resp.set_status(500, "Internal Server Error");
      resp.set_content_type("application/json");
      resp.body = R"({"error":"no chunks"})";
      resp.set_content_length(resp.body.size());
      return;
    }
    std::string body_data;
    body_data.reserve(record->file_size);
    for (const auto& c : chunks) {
      auto data = fs.read_file("chunks/" + c.chunk_hash);
      if (!data) {
        resp.set_status(500, "Internal Server Error");
        resp.set_content_type("application/json");
        resp.body = R"({"error":"chunk read failed"})";
        resp.set_content_length(resp.body.size());
        return;
      }
      body_data.append(data->data(), data->size());
    }
    resp.set_status(200, "OK");
    resp.set_content_type("application/octet-stream");
    resp.body = std::move(body_data);
    resp.set_content_length(resp.body.size());
    resp.set_header("Content-Disposition", build_attachment_content_disposition(record->file_name));
  });

  server.get("/api/users/:id", [&db](const HttpRequest& req, HttpResponse& resp) {
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp.set_status(400, "Bad Request");
      resp.body = R"({"error":"missing id"})";
      resp.set_content_length(resp.body.size());
      resp.set_content_type("application/json");
      return;
    }
    auto user = db.get_user(std::stoll(it->second));
    if (!user) {
      resp.set_status(404, "Not Found");
      resp.body = R"({"error":"user not found"})";
      resp.set_content_length(resp.body.size());
      resp.set_content_type("application/json");
      return;
    }
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    resp.body = R"({"user_id":)" + std::to_string(user->user_id) + R"(,"username":")" + user->username + R"("})";
    resp.set_content_length(resp.body.size());
  });

  server.ws("/ws", [](const HttpRequest& req, std::shared_ptr<WsConnection> ws_conn) {
    Logger::_info("WebSocket 连接已建立: " + req.path);
    ws_conn->set_message_handler([](const WsFrame& frame) {
      Logger::_info("WebSocket 收到帧: opcode=" + std::to_string(static_cast<int>(frame.opcode)) +
                    ", payload_size=" + std::to_string(frame.payload.size()));
    });
    ws_conn->set_close_handler(
      [](uint16_t code) { Logger::_info("WebSocket 连接关闭: code=" + std::to_string(code)); });
  });
}

} // namespace hps

int main(int argc, char* argv[]) {
  hps::Logger::init("music-server");

  auto& logger = hps::Logger::getInstance();
  auto file_appender = std::make_shared<hps::FileLogAppender>("server.log");
  auto formatter = std::make_shared<hps::LogFormatter>("%d{%Y-%m-%d %H:%M:%S} [%p] [%t] %f:%l %m%n");
  file_appender->setFormatter(formatter);
  logger.addAppender(file_appender);
  logger.setLevel(hps::LogLevel::INFO);

  hps::g_start_time = std::chrono::steady_clock::now();

  hps::ServerConfig cfg;
  try {
    cfg = hps::load_config(argc, argv);
  } catch (const std::exception& e) {
    hps::Logger::_error("配置加载失败: " + std::string(e.what()));
    hps::Logger::shutdown();
    return 1;
  }

  hps::Logger::_info("配置加载完成，端口: " + std::to_string(cfg.port) +
                     ", 线程数: " + std::to_string(cfg.thread_count));

  auto db = std::make_unique<hps::DatabasePool>(
    []() -> std::unique_ptr<hps::IConnection> { return std::make_unique<hps::BoostMySqlConnection>(); });
  if (!db->init(cfg.db)) {
    hps::Logger::_error("数据库连接池初始化失败");
    hps::Logger::shutdown();
    return 1;
  }
  hps::Logger::_info("数据库连接池已初始化");

  auto auth = hps::create_auth_service(*db, cfg.auth_secret);
  hps::Logger::_info("认证服务已初始化");

  auto fs = std::make_unique<hps::FileSystem>(cfg.fs_root_dir);
  hps::Logger::_info("文件系统已初始化，根目录: " + cfg.fs_root_dir);

  if (!fs->store_file("chunks/.keep", {})) {
    hps::Logger::_warn("无法创建 chunks 目录");
  }

  hps::TcpServer::Config tcp_cfg;
  tcp_cfg.port = cfg.port;
  tcp_cfg.thread_count = cfg.thread_count;
  tcp_cfg.backlog = cfg.backlog;
  tcp_cfg.epoll_timeout_ms = cfg.epoll_timeout_ms;
  tcp_cfg.ssl_config = cfg.ssl;

  hps::HttpServer server(tcp_cfg);
  server.set_auth_service(*auth);
  hps::register_routes(server, *db, *fs, *auth, cfg);

  if (!server.init()) {
    hps::Logger::_error("HTTP 服务器初始化失败");
    db->close();
    hps::Logger::shutdown();
    return 1;
  }
  hps::Logger::_info("HTTP 服务器已初始化，绑定端口: " + std::to_string(server.actual_port()));

  hps::Logger::_info("HTTP 服务器启动，监听端口: " + std::to_string(server.actual_port()));

  server.start();

  hps::Logger::_info("正在关闭数据库连接池...");
  db->close();

  hps::Logger::_info("服务器已停止");
  hps::Logger::shutdown();
  return 0;
}
