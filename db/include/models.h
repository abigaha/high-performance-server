#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hps {

enum class UserRole : uint8_t { GUEST = 0, NORMAL = 1, VIP = 2 };

struct User {
  int64_t user_id{0};
  std::string username;
  std::string password_hash;
  UserRole role{UserRole::GUEST};
  std::string email;
  std::string created_at;
};

struct FileRecord {
  int64_t file_id{0};
  std::string file_name;
  std::string file_hash;
  std::size_t file_size{0};
  std::string content_type;
  int chunk_size{2097152};
  std::string created_at;
};

struct FileChunkRecord {
  std::string file_hash;
  int chunk_index{0};
  std::string chunk_hash;
  std::size_t chunk_offset{0};
  int chunk_size{0};
};

struct AuthUser {
  int64_t user_id{0};
  std::string username;
  UserRole role{UserRole::GUEST};
};

} // namespace hps
