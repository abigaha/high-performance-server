#include "logger.h"
#include "main_functions.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace hps {

namespace {

void parse_server_section(const nlohmann::json& json, ServerConfig& cfg) {
  if (!json.contains("server")) {
    return;
  }
  const auto& server = json["server"];
  if (server.contains("port")) {
    cfg.port = server["port"].get<uint16_t>();
  }
  if (server.contains("thread_count")) {
    cfg.thread_count = server["thread_count"].get<size_t>();
  }
  if (server.contains("backlog")) {
    cfg.backlog = server["backlog"].get<size_t>();
  }
  if (server.contains("epoll_timeout_ms")) {
    cfg.epoll_timeout_ms = server["epoll_timeout_ms"].get<int>();
  }
  if (server.contains("auth_secret")) {
    cfg.auth_secret = server["auth_secret"].get<std::string>();
  }
  if (server.contains("normal_max_size")) {
    cfg.normal_max_size = server["normal_max_size"].get<int>();
  }
  if (server.contains("vip_max_size")) {
    cfg.vip_max_size = server["vip_max_size"].get<int>();
  }
}

void parse_database_section(const nlohmann::json& json, ServerConfig& cfg) {
  if (!json.contains("database")) {
    return;
  }
  const auto& database = json["database"];
  if (database.contains("host")) {
    cfg.db.host = database["host"].get<std::string>();
  }
  if (database.contains("port")) {
    cfg.db.port = database["port"].get<uint16_t>();
  }
  if (database.contains("username")) {
    cfg.db.username = database["username"].get<std::string>();
  }
  if (database.contains("password")) {
    cfg.db.password = database["password"].get<std::string>();
  }
  if (database.contains("database")) {
    cfg.db.database = database["database"].get<std::string>();
  }
  if (database.contains("pool_size")) {
    cfg.db.pool_size = database["pool_size"].get<size_t>();
  }
  if (database.contains("connect_timeout_ms")) {
    cfg.db.connect_timeout_ms = database["connect_timeout_ms"].get<uint32_t>();
  }
  if (database.contains("read_timeout_ms")) {
    cfg.db.read_timeout_ms = database["read_timeout_ms"].get<uint32_t>();
  }
}

void parse_ssl_section(const nlohmann::json& json, ServerConfig& cfg) {
  if (!json.contains("ssl")) {
    return;
  }
  const auto& ssl = json["ssl"];
  if (ssl.contains("enabled")) {
    cfg.ssl.enabled = ssl["enabled"].get<bool>();
  }
  if (ssl.contains("cert_file")) {
    cfg.ssl.cert_file = ssl["cert_file"].get<std::string>();
  }
  if (ssl.contains("key_file")) {
    cfg.ssl.key_file = ssl["key_file"].get<std::string>();
  }
  if (ssl.contains("ca_file")) {
    cfg.ssl.ca_file = ssl["ca_file"].get<std::string>();
  }
  if (ssl.contains("verify_peer")) {
    cfg.ssl.verify_peer = ssl["verify_peer"].get<bool>();
  }
}

void parse_fs_section(const nlohmann::json& json, ServerConfig& cfg) {
  if (!json.contains("filesystem")) {
    return;
  }
  const auto& filesystem = json["filesystem"];
  if (filesystem.contains("root_dir")) {
    cfg.fs_root_dir = filesystem["root_dir"].get<std::string>();
  }
}

std::string find_config_path(int argc, char** argv) {
  std::string config_path = "config.json";
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    }
  }
  return config_path;
}

void parse_cmd_args(int argc, char** argv, ServerConfig& cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      cfg.port = static_cast<uint16_t>(std::stoul(argv[++i]));
    } else if (arg == "--config" && i + 1 < argc) {
      ++i;
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
  std::ifstream file(path);
  if (!file.is_open()) {
    Logger::_info("未找到配置文件 " + path + "，使用默认配置");
    return;
  }
  try {
    const auto json = nlohmann::json::parse(file);
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
  if (const char* env = std::getenv("AUTH_SECRET")) {
    cfg.auth_secret = env;
  }
}

} // namespace

ServerConfig load_config(int argc, char** argv) {
  ServerConfig cfg;
  const auto config_path = find_config_path(argc, argv);
  parse_json_file(config_path, cfg);
  apply_env_overrides(cfg);
  parse_cmd_args(argc, argv, cfg);
  if (cfg.auth_secret.empty()) {
    throw std::runtime_error("认证密钥未配置：请设置 AUTH_SECRET 或 server.auth_secret");
  }
  cfg.threads = static_cast<int>(cfg.thread_count);
  cfg.db_host = cfg.db.host;
  cfg.db_port = cfg.db.port;
  cfg.db_user = cfg.db.username;
  cfg.db_password = cfg.db.password;
  cfg.db_name = cfg.db.database;
  cfg.data_dir = cfg.fs_root_dir;
  return cfg;
}

} // namespace hps
