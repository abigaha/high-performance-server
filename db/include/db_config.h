#pragma once

#include <cstdint>
#include <string>

namespace hps {

struct DbConfig {
  std::string host = "127.0.0.1";
  uint16_t port = 3306;
  std::string username = "root";
  std::string password;
  std::string database = "music_server";
  std::size_t pool_size = 10;
  uint32_t connect_timeout_ms = 3000;
  uint32_t read_timeout_ms = 5000;
};

} // namespace hps
