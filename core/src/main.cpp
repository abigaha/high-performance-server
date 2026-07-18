#include "auth_middleware.h"
#include "auth_service.h"
#include "boost_mysql_connection.h"
#include "database_pool.h"
#include "file_system.h"
#include "http_server.h"
#include "logappender.h"
#include "logformatter.h"
#include "logger.h"
#include "ssl_context.h"
#include "ws_connection.h"

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hps {
namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::chrono::steady_clock::time_point g_start_time;

struct ServerConfig {
  uint16_t port{8080};
  size_t thread_count{4};
  size_t backlog{128};
  int epoll_timeout_ms{100};
  DbConfig db;
  SslConfig ssl;
  std::string fs_root_dir = "./data";
  std::string auth_secret = "hps-server-secret-2024";
  int normal_max_size = 10 * 1024 * 1024;
  int vip_max_size = 100 * 1024 * 1024;
};

void parse_server_section(const nlohmann::json& json, ServerConfig& cfg) {
  if (!json.contains("server")) {
    return;
  }
  const auto& s = json["server"];
  if (s.contains("port")) {
    cfg.port = s["port"].get<uint16_t>();
  }
  if (s.contains("thread_count")) {
    cfg.thread_count = s["thread_count"].get<size_t>();
  }
  if (s.contains("backlog")) {
    cfg.backlog = s["backlog"].get<size_t>();
  }
  if (s.contains("epoll_timeout_ms")) {
    cfg.epoll_timeout_ms = s["epoll_timeout_ms"].get<int>();
  }
  if (s.contains("auth_secret")) {
    cfg.auth_secret = s["auth_secret"].get<std::string>();
  }
  if (s.contains("normal_max_size")) {
    cfg.normal_max_size = s["normal_max_size"].get<int>();
  }
  if (s.contains("vip_max_size")) {
    cfg.vip_max_size = s["vip_max_size"].get<int>();
  }
}

void parse_database_section(const nlohmann::json& json, ServerConfig& cfg) {
  if (!json.contains("database")) {
    return;
  }
  const auto& d = json["database"];
  if (d.contains("host")) {
    cfg.db.host = d["host"].get<std::string>();
  }
  if (d.contains("port")) {
    cfg.db.port = d["port"].get<uint16_t>();
  }
  if (d.contains("username")) {
    cfg.db.username = d["username"].get<std::string>();
  }
  if (d.contains("password")) {
    cfg.db.password = d["password"].get<std::string>();
  }
  if (d.contains("database")) {
    cfg.db.database = d["database"].get<std::string>();
  }
  if (d.contains("pool_size")) {
    cfg.db.pool_size = d["pool_size"].get<size_t>();
  }
  if (d.contains("connect_timeout_ms")) {
    cfg.db.connect_timeout_ms = d["connect_timeout_ms"].get<uint32_t>();
  }
  if (d.contains("read_timeout_ms")) {
    cfg.db.read_timeout_ms = d["read_timeout_ms"].get<uint32_t>();
  }
}

void parse_ssl_section(const nlohmann::json& json, ServerConfig& cfg) {
  if (!json.contains("ssl")) {
    return;
  }
  const auto& s = json["ssl"];
  if (s.contains("enabled")) {
    cfg.ssl.enabled = s["enabled"].get<bool>();
  }
  if (s.contains("cert_file")) {
    cfg.ssl.cert_file = s["cert_file"].get<std::string>();
  }
  if (s.contains("key_file")) {
    cfg.ssl.key_file = s["key_file"].get<std::string>();
  }
  if (s.contains("ca_file")) {
    cfg.ssl.ca_file = s["ca_file"].get<std::string>();
  }
  if (s.contains("verify_peer")) {
    cfg.ssl.verify_peer = s["verify_peer"].get<bool>();
  }
}

void parse_fs_section(const nlohmann::json& json, ServerConfig& cfg) {
  if (!json.contains("filesystem")) {
    return;
  }
  const auto& fs = json["filesystem"];
  if (fs.contains("root_dir")) {
    cfg.fs_root_dir = fs["root_dir"].get<std::string>();
  }
}

void parse_cmd_args(int argc, char** argv, ServerConfig& cfg, std::string& config_path) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      cfg.port = static_cast<uint16_t>(std::stoul(argv[++i]));
    } else if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--threads" && i + 1 < argc) {
      cfg.thread_count = std::stoul(argv[++i]);
    } else if (arg == "--db-host" && i + 1 < argc) {
      cfg.db.host = argv[++i];
    } else if (arg == "--db-port" && i + 1 < argc) {
      cfg.db.port = static_cast<uint16_t>(std::stoul(argv[++i]));
    } else if (arg == "--data-dir" && i + 1 < argc) {
      cfg.fs_root_dir = argv[++i];
    } else if (arg == "--ssl-cert" && i + 1 < argc) {
      cfg.ssl.cert_file = argv[++i];
      cfg.ssl.enabled = true;
    } else if (arg == "--ssl-key" && i + 1 < argc) {
      cfg.ssl.key_file = argv[++i];
      cfg.ssl.enabled = true;
    } else if (arg == "--ssl-ca" && i + 1 < argc) {
      cfg.ssl.ca_file = argv[++i];
    } else if (arg == "--ssl-verify") {
      cfg.ssl.verify_peer = true;
    } else if (arg == "--help") {
      std::cout << "用法: high-performance-server [选项]\n"
                << "  --port <port>         监听端口 (默认 8080)\n"
                << "  --config <path>       配置文件路径 (默认 config.json)\n"
                << "  --threads <n>         工作线程数 (默认 4)\n"
                << "  --db-host <host>      数据库主机\n"
                << "  --db-port <port>      数据库端口\n"
                << "  --data-dir <dir>      数据存储目录\n"
                << "  --ssl-cert <path>     SSL 证书路径 (同时启用 SSL)\n"
                << "  --ssl-key <path>      SSL 密钥路径 (同时启用 SSL)\n"
                << "  --ssl-ca <path>       SSL CA 证书路径\n"
                << "  --ssl-verify          启用客户端证书验证\n"
                << "  --help                显示此帮助\n";
      std::exit(0);
    }
  }
}

void parse_json_file(const std::string& path, ServerConfig& cfg) {
  std::ifstream f(path);
  if (!f.is_open()) {
    Logger::_info("未找到配置文件 " + path + "，使用默认配置");
    return;
  }
  try {
    auto json = nlohmann::json::parse(f);
    parse_server_section(json, cfg);
    parse_database_section(json, cfg);
    parse_ssl_section(json, cfg);
    parse_fs_section(json, cfg);
  } catch (const std::exception& e) {
    Logger::_warn("配置文件解析失败: " + std::string(e.what()) + "，使用默认配置");
  }
}

void apply_env_overrides(ServerConfig& cfg) {
  if (const char* env = std::getenv("DB_HOST")) {
    cfg.db.host = env;
  }
  if (const char* env = std::getenv("DB_PORT")) {
    cfg.db.port = static_cast<uint16_t>(std::stoul(env));
  }
  if (const char* env = std::getenv("SERVER_PORT")) {
    cfg.port = static_cast<uint16_t>(std::stoul(env));
  }
}

ServerConfig load_config(int argc, char** argv) {
  ServerConfig cfg;
  std::string config_path = "config.json";
  parse_json_file(config_path, cfg);
  apply_env_overrides(cfg);
  parse_cmd_args(argc, argv, cfg, config_path);
  return cfg;
}

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

bool check_size_limit(const HttpRequest& req, const ServerConfig& cfg, std::size_t file_size, HttpResponse& resp) {
  int limit = 0;
  if (req.auth_user.role == UserRole::NORMAL) {
    limit = cfg.normal_max_size;
  } else if (req.auth_user.role == UserRole::VIP) {
    limit = cfg.vip_max_size;
  }
  if (limit > 0 && file_size > static_cast<std::size_t>(limit)) {
    resp.set_status(413, "Payload Too Large");
    resp.set_content_type("application/json");
    resp.body = R"({"error":"文件大小超过限制 ")" + std::to_string(limit / (1024 * 1024)) + R"(MB"})";
    resp.set_content_length(resp.body.size());
    return false;
  }
  return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void register_routes(HttpServer& server,
                     DatabasePool& db,
                     FileSystem& fs,
                     IAuthService& auth,
                     const ServerConfig& cfg) {
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
    int offset = 0;
    int limit = 20;
    auto q = req.query_string;
    auto npos = q.find("name=");
    if (npos != std::string::npos) {
      auto end = q.find('&', npos);
      name_pattern = q.substr(npos + 5, end == std::string::npos ? end : end - npos - 5);
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
    auto records = db.search_files(name_pattern, offset, limit);
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    std::string body = R"({"files":[)";
    for (size_t i = 0; i < records.size(); ++i) {
      if (i > 0) {
        body += ",";
      }
      body += R"({"file_id":)" + std::to_string(records[i].file_id) + R"(,"file_name":")" + records[i].file_name +
              R"(","file_hash":")" + records[i].file_hash + R"(","file_size":)" + std::to_string(records[i].file_size) +
              R"(})";
    }
    body += R"(],"offset":)" + std::to_string(offset) + R"(,"limit":)" + std::to_string(limit) + R"(})";
    resp.body = body;
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
    resp.body = R"({"file_id":)" + std::to_string(record->file_id) + R"(,"file_name":")" + record->file_name +
                R"(","file_hash":")" + record->file_hash + R"(","file_size":)" + std::to_string(record->file_size) +
                R"(,"content_type":")" + record->content_type + R"("})";
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
    resp.set_header("Content-Disposition", "attachment; filename=\"" + record->file_name + "\"");
  });

  auto upload_setup = [&fs](const HttpRequest&, UploadStreamContext& ctx, HttpParser&) -> void {
    (void)fs;
    ctx.store_chunk_data = [&fs](std::string_view data, const std::string& chunk_hash) -> bool {
      std::vector<char> buf(data.begin(), data.end());
      return fs.store_file("chunks/" + chunk_hash, buf);
    };
  };

  server.upload(
    "/api/files/upload",
    [&db, &cfg](const HttpRequest& req, UploadStreamContext& ctx, HttpResponse& resp) {
      if (!check_auth(req, resp, UserRole::NORMAL)) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx.hash_ctx));
        return;
      }
      if (!check_size_limit(req, cfg, ctx.total_size, resp)) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx.hash_ctx));
        return;
      }

      auto* md_ctx = static_cast<EVP_MD_CTX*>(ctx.hash_ctx);
      std::array<unsigned char, EVP_MAX_MD_SIZE> final_hash{};
      unsigned int hash_len = 0;
      EVP_DigestFinal_ex(md_ctx, final_hash.data(), &hash_len);
      EVP_MD_CTX_free(md_ctx);
      ctx.hash_ctx = nullptr;

      auto overall_hash = FileSystem::sha256_hex(reinterpret_cast<const char*>(final_hash.data()), hash_len);
      ctx.file_hash = overall_hash;

      for (auto& c : ctx.chunks) {
        c.file_hash = overall_hash;
      }

      auto existing = db.get_file_record_by_hash(overall_hash);
      if (existing) {
        resp.set_status(200, "OK");
        resp.set_content_type("application/json");
        resp.body = R"({"file_id":)" + std::to_string(existing->file_id) + R"(,"file_name":")" + existing->file_name +
                    R"(","file_hash":")" + overall_hash + R"(","size":)" + std::to_string(ctx.total_size) +
                    R"(,"exists":true})";
        resp.set_content_length(resp.body.size());
        return;
      }

      FileRecord record;
      record.file_name = ctx.file_name;
      record.file_hash = overall_hash;
      record.file_size = ctx.total_size;
      record.content_type = req.headers.contains("Content-Type") ? req.headers.at("Content-Type")
                                                                 : "application/octet-stream";
      record.chunk_size = 2097152;
      db.store_file_record(record);
      db.store_file_chunks(ctx.chunks);

      resp.set_status(201, "Created");
      resp.set_content_type("application/json");
      resp.body = R"({"file_id":)" + std::to_string(record.file_id) + R"(,"file_name":")" + record.file_name +
                  R"(","file_hash":")" + overall_hash + R"(","size":)" + std::to_string(ctx.total_size) +
                  R"(,"chunks":)" + std::to_string(ctx.chunks.size()) + R"(})";
      resp.set_content_length(resp.body.size());
    },
    upload_setup);

  server.get("/api/files/:hash/download", [&db, &fs](const HttpRequest& req, HttpResponse& resp) {
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
    ws_conn->set_message_handler([](WsFrame frame) {
      Logger::_info("WebSocket 收到帧: opcode=" + std::to_string(static_cast<int>(frame.opcode)) +
                    ", payload_size=" + std::to_string(frame.payload.size()));
    });
    ws_conn->set_close_handler(
      [](uint16_t code) { Logger::_info("WebSocket 连接关闭: code=" + std::to_string(code)); });
  });
}

} // anonymous namespace
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

  auto cfg = hps::load_config(argc, argv);

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
