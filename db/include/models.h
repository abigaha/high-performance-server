#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hps {

struct User {
  int64_t user_id{0};
  std::string username;
  std::string password_hash;
  std::string email;
  std::string created_at;
};

struct DownloadLog {
  int64_t log_id{0};
  int64_t user_id{0};
  std::string file_hash;
  std::string downloaded_at;
};

struct FileMeta {
  std::string file_hash;
  std::string file_path;
  std::size_t file_size{0};
  std::string content_type;
  std::string created_at;
};

} // namespace hps
