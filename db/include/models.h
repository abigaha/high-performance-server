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
  std::string salt;
  UserRole role{UserRole::GUEST};
  std::string email;
  std::string created_at;
};

struct FileRecord {
  int64_t file_id{0};
  int64_t music_id{0};
  std::string file_name;
  std::string file_hash;
  std::size_t file_size{0};
  std::string content_type;
  int chunk_size{2097152};
  int64_t uploaded_by{0};
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

struct MusicMeta {
  int64_t music_id{0};
  std::string title;
  std::string artist;
  std::string album;
  std::string genre;
  int duration_sec{0};
  int track_number{0};
  std::string created_at;
  std::string updated_at;
};

struct Playlist {
  int64_t playlist_id{0};
  int64_t user_id{0};
  std::string name;
  std::string description;
  int item_count{0};
  std::string created_at;
};

struct PlaylistItem {
  int64_t id{0};
  int64_t playlist_id{0};
  int64_t music_id{0};
  std::string title;
  std::string artist;
  std::string file_hash;
  int sort_order{0};
  std::string added_at;
};

} // namespace hps
