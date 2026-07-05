#include "database_pool.h"
#include "file_system.h"
#include "http_server.h"
#include "logappender.h"
#include "logformatter.h"
#include "logger.h"
#include "mock_connection.h"
#include "ssl_context.h"
#include "ws_connection.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

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

ServerConfig load_config(int argc, char** argv) {
  ServerConfig cfg;
  std::string config_path = "config.json";
  parse_cmd_args(argc, argv, cfg, config_path);
  parse_json_file(config_path, cfg);
  return cfg;
}

void register_routes(HttpServer& server, DatabasePool& db, FileSystem& /*fs*/) {
  server.get("/api/health", [](const HttpRequest& /*req*/, HttpResponse& resp) {
    auto uptime =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_start_time).count();
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    resp.body = R"({"status":"ok","uptime":)" + std::to_string(uptime) + R"(})";
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

  server.post("/api/users", [&db](const HttpRequest& /*req*/, HttpResponse& resp) {
    User u;
    u.username = "mock_user";
    u.password_hash = "mock_hash";
    u.email = "mock@example.com";
    if (db.create_user(u)) {
      resp.set_status(201, "Created");
      resp.body = R"({"status":"created"})";
    } else {
      resp.set_status(500, "Internal Server Error");
      resp.body = R"({"error":"create failed"})";
    }
    resp.set_content_type("application/json");
    resp.set_content_length(resp.body.size());
  });

  server.get("/api/users/:id/history", [&db](const HttpRequest& req, HttpResponse& resp) {
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
      resp.set_status(400, "Bad Request");
      resp.body = R"({"error":"missing id"})";
      resp.set_content_length(resp.body.size());
      resp.set_content_type("application/json");
      return;
    }
    auto logs = db.get_download_history(std::stoll(it->second));
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    std::string body = R"({"downloads":[)";
    for (size_t i = 0; i < logs.size(); ++i) {
      if (i > 0) {
        body += ",";
      }
      body += R"({"log_id":)" + std::to_string(logs[i].log_id) + R"(,"file_hash":")" + logs[i].file_hash + R"("})";
    }
    body += "]}";
    resp.body = body;
    resp.set_content_length(resp.body.size());
  });

  server.get("/api/files/:hash", [&db](const HttpRequest& req, HttpResponse& resp) {
    auto it = req.path_params.find("hash");
    if (it == req.path_params.end()) {
      resp.set_status(400, "Bad Request");
      resp.body = R"({"error":"missing hash"})";
      resp.set_content_length(resp.body.size());
      resp.set_content_type("application/json");
      return;
    }
    auto meta = db.get_file_meta(it->second);
    if (!meta) {
      resp.set_status(404, "Not Found");
      resp.body = R"({"error":"file not found"})";
      resp.set_content_length(resp.body.size());
      resp.set_content_type("application/json");
      return;
    }
    resp.set_status(200, "OK");
    resp.set_content_type("application/json");
    resp.body = R"({"file_hash":")" + meta->file_hash + R"(,"file_path":")" + meta->file_path + R"(,"file_size":)" +
                std::to_string(meta->file_size) + R"(})";
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
    []() -> std::unique_ptr<hps::IConnection> { return std::make_unique<hps::MockConnection>(); });
  if (!db->init(cfg.db)) {
    hps::Logger::_error("数据库连接池初始化失败");
    hps::Logger::shutdown();
    return 1;
  }
  hps::Logger::_info("数据库连接池已初始化");

  auto fs = std::make_unique<hps::FileSystem>(cfg.fs_root_dir);
  hps::Logger::_info("文件系统已初始化，根目录: " + cfg.fs_root_dir);

  hps::TcpServer::Config tcp_cfg;
  tcp_cfg.port = cfg.port;
  tcp_cfg.thread_count = cfg.thread_count;
  tcp_cfg.backlog = cfg.backlog;
  tcp_cfg.epoll_timeout_ms = cfg.epoll_timeout_ms;
  tcp_cfg.ssl_config = cfg.ssl;

  hps::HttpServer server(tcp_cfg);
  hps::register_routes(server, *db, *fs);

  if (!server.init()) {
    hps::Logger::_error("HTTP 服务器初始化失败");
    db->close();
    hps::Logger::shutdown();
    return 1;
  }
  hps::Logger::_info("HTTP 服务器已初始化，绑定端口: " + std::to_string(server.actual_port()));

  hps::Logger::_info("HTTP 服务器启动，监听端口: " + std::to_string(server.actual_port()));

  // 信号处理由 TcpServer::init() 中的 sigaction 完成，
  // SIGINT/SIGTERM → TcpServer::signal_handler → stop() → event_loop 退出
  server.start();

  hps::Logger::_info("正在关闭数据库连接池...");
  db->close();

  hps::Logger::_info("服务器已停止");
  hps::Logger::shutdown();
  return 0;
}
