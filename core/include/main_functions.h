#pragma once

#include "db_config.h"
#include "ssl_context.h"

#include <cstdint>
#include <string>

namespace hps {

struct ServerConfig {
  uint16_t port{8080};
  int threads{4};
  std::string db_host = "127.0.0.1";
  int db_port = 3306;
  std::string db_user = "root";
  std::string db_password;
  std::string db_name = "music_server";
  std::string data_dir = "./data";

  size_t thread_count{4};
  size_t backlog{128};
  int epoll_timeout_ms{100};
  DbConfig db;
  SslConfig ssl;
  std::string fs_root_dir = "./data";
  std::string auth_secret;
  int normal_max_size = 10 * 1024 * 1024;
  int vip_max_size = 100 * 1024 * 1024;
};

ServerConfig load_config(int argc, char** argv);
} // namespace hps
