#include "admin_bootstrap.h"
#include "auth_middleware.h"
#include "auth_routes.h"
#include "auth_service.h"
#include "authorization.h"
#include "boost_mysql_connection.h"
#include "database_pool.h"
#include "email_validation.h"
#include "file_routes.h"
#include "file_system.h"
#include "http_server.h"
#include "i_http_server.h"
#include "idatabase_pool.h"
#include "logappender.h"
#include "logformatter.h"
#include "logger.h"
#include "main_functions.h"
#include "pending_chunk_deletions.h"
#include "playlist_routes.h"
#include "range_parser.h"
#include "schema_migrations.h"
#include "ssl_context.h"
#include "stream_download_utils.h"
#include "upload_policy.h"
#include "upload_setup.h"
#include "vip_admin_routes.h"
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

HttpResponse auth_error(int status, const std::string& msg, std::string code = {}) {
  HttpResponse resp;
  if (status == 400) {
    resp.set_status(status, "Bad Request");
  } else if (status == 401) {
    resp.set_status(status, "Unauthorized");
  } else if (status == 403) {
    resp.set_status(status, "Forbidden");
  } else if (status == 404) {
    resp.set_status(status, "Not Found");
  } else if (status == 409) {
    resp.set_status(status, "Conflict");
  } else {
    resp.set_status(status, "Internal Server Error");
  }
  resp.set_content_type("application/json");
  if (code.empty()) {
    if (status == 400) {
      code = "INVALID_REQUEST";
    } else if (status == 401) {
      code = "AUTH_REQUIRED";
    } else if (status == 403) {
      code = "FORBIDDEN";
    } else if (status == 404) {
      code = "NOT_FOUND";
    } else {
      code = "PERSISTENCE_ERROR";
    }
  }
  resp.body = nlohmann::json{{"code", std::move(code)}, {"error", msg}}.dump();
  resp.set_content_length(resp.body.size());
  return resp;
}

bool check_auth(const HttpRequest& req, HttpResponse& resp, Capability capability) {
  if (req.auth_status == TokenValidationStatus::STORAGE_ERROR) {
    resp = auth_error(500, "认证存储暂时不可用", "PERSISTENCE_ERROR");
    return false;
  }
  if (!has_capability(req.auth_user, Capability::USE_AUTHENTICATED_FEATURES)) {
    resp = auth_error(401, "需要登录");
    return false;
  }
  if (!has_capability(req.auth_user, capability)) {
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
                     ChunkLifecycleCoordinator& chunk_lifecycle,
                     IAuthService& auth,
                     const ServerConfig& cfg) {
  register_auth_routes(server, db, auth);
  register_vip_routes(server, db, [] { return std::chrono::system_clock::now(); });
  register_admin_routes(server, db, [] { return std::chrono::system_clock::now(); });
  register_file_routes(server, db, fs, chunk_lifecycle);
  register_playlist_routes(server, db);

  server.get("/api/health", [](const HttpRequest&, HttpResponse& resp) {
    auto uptime =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_start_time).count();
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    resp.body = R"({"status":"ok","uptime":)" + std::to_string(uptime) + R"(})";
    resp.set_content_length(resp.body.size());
  });

  server.get("/api/files/:id/download", [&db, &fs](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, Capability::USE_AUTHENTICATED_FEATURES)) {
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
    if (!check_auth(req, resp, Capability::USE_AUTHENTICATED_FEATURES)) {
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
    if (!check_auth(req, resp, Capability::USE_AUTHENTICATED_FEATURES)) {
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

  // M1 — 音乐库搜索
  server.get("/api/music/library", [&db](const HttpRequest& req, HttpResponse& resp) {
    if (!check_auth(req, resp, Capability::USE_AUTHENTICATED_FEATURES)) {
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
    if (!check_auth(req, resp, Capability::USE_AUTHENTICATED_FEATURES)) {
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

  auto upload_setup = make_upload_setup(db, fs, chunk_lifecycle);

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
      if (!check_auth(req, resp, Capability::USE_AUTHENTICATED_FEATURES)) {
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
    if (!check_auth(req, resp, Capability::USE_AUTHENTICATED_FEATURES)) {
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

  const auto migration_result = hps::run_schema_migrations(*db, std::chrono::system_clock::now());
  if (migration_result.status != hps::MutationStatus::OK) {
    hps::Logger::_error("数据库结构迁移失败: " + migration_result.detail.value_or("unknown_stage"));
    db->close();
    hps::Logger::shutdown();
    return 1;
  }
  hps::Logger::_info("数据库结构迁移完成");

  hps::ChunkLifecycleCoordinator chunk_lifecycle;
  if (!db->bind_chunk_lifecycle_coordinator(chunk_lifecycle)) {
    hps::Logger::_error("数据库连接池绑定文件生命周期协调器失败");
    db->close();
    hps::Logger::shutdown();
    return 1;
  }
  auto fs = std::make_unique<hps::FileSystem>(cfg.fs_root_dir);
  hps::Logger::_info("文件系统已初始化，根目录: " + cfg.fs_root_dir);

  if (!fs->store_file("chunks/.keep", {})) {
    hps::Logger::_warn("无法创建 chunks 目录");
  }

  for (;;) {
    const auto cleanup = hps::run_pending_chunk_deletions(*db, *fs, chunk_lifecycle, 100);
    if (cleanup.status != hps::MutationStatus::OK) {
      hps::Logger::_warn("startup pending chunk cleanup deferred");
      break;
    }
    if (!cleanup.value || *cleanup.value == 0) {
      break;
    }
  }

  const auto admin_result = hps::bootstrap_admin(*db, cfg.admin);
  if (admin_result.status != hps::MutationStatus::OK) {
    hps::Logger::_error("管理员引导失败: " + admin_result.detail.value_or("ADMIN_BOOTSTRAP_FAILED"));
    db->close();
    hps::Logger::shutdown();
    return 1;
  }
  hps::Logger::_info("管理员引导检查完成");

  auto auth = hps::create_auth_service(*db, cfg.auth_secret);
  hps::Logger::_info("认证服务已初始化");

  hps::TcpServer::Config tcp_cfg;
  tcp_cfg.port = cfg.port;
  tcp_cfg.thread_count = cfg.thread_count;
  tcp_cfg.backlog = cfg.backlog;
  tcp_cfg.epoll_timeout_ms = cfg.epoll_timeout_ms;
  tcp_cfg.ssl_config = cfg.ssl;

  hps::HttpServer server(tcp_cfg);
  server.set_auth_service(*auth);
  hps::register_routes(server, *db, *fs, chunk_lifecycle, *auth, cfg);

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
