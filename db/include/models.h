#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hps {

inline constexpr std::size_t kMaximumEmailLength = 128;

enum class UserRole : uint8_t { GUEST = 0, NORMAL = 1, VIP = 2, ADMIN = 3 };

enum class VipStatus : uint8_t { NONE, ACTIVE, EXPIRED };

enum class MutationStatus : uint8_t {
  OK,
  NOT_FOUND,
  USER_NOT_FOUND,
  OWNER_REQUIRED,
  CONFLICT,
  INVALID_STATE,
  STORAGE_ERROR,
};

template <typename T>
struct MutationResult {
  MutationStatus status{MutationStatus::STORAGE_ERROR};
  std::optional<T> value;
  std::optional<std::string> detail;
};

enum class LookupStatus : uint8_t {
  FOUND,
  NOT_FOUND,
  STORAGE_ERROR,
  INVALID_DATA,
};

template <typename T>
struct LookupResult {
  LookupStatus status{LookupStatus::STORAGE_ERROR};
  std::optional<T> value;
};

struct User {
  int64_t user_id{0};
  std::string username;
  std::string password_hash;
  std::string salt;
  UserRole role{UserRole::GUEST};
  std::string email;
  std::optional<std::chrono::system_clock::time_point> vip_expires_at;
  std::string created_at;
};

inline bool has_valid_vip_expiry_state(const User& user) noexcept {
  return (user.role == UserRole::VIP) == user.vip_expires_at.has_value();
}

struct AdminUserPage {
  std::vector<User> items;
  int total{0};
  int offset{0};
  int limit{20};
};

struct EffectiveIdentity {
  int64_t user_id{0};
  std::string username;
  UserRole role{UserRole::GUEST};
  VipStatus vip_status{VipStatus::NONE};
  std::optional<std::chrono::system_clock::time_point> vip_expires_at;
};

// 认证期间已读取的可公开用户资料。请求上下文只保留序列化响应所需字段，
// 避免保存密码散列或盐值，并允许 /api/auth/me 复用认证查询结果。
struct AuthenticatedUserProfile {
  int64_t user_id{0};
  std::string username;
  std::string email;
  std::string created_at;
};

enum class TokenValidationStatus : uint8_t {
  AUTHENTICATED,
  INVALID,
  USER_NOT_FOUND,
  STORAGE_ERROR,
};

struct TokenValidationResult {
  TokenValidationStatus status{TokenValidationStatus::INVALID};
  EffectiveIdentity identity;
  std::optional<AuthenticatedUserProfile> profile;
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

struct FilePage {
  std::vector<FileRecord> items;
  int total{0};
  int offset{0};
  int limit{20};
};

struct FileChunkRecord {
  std::string file_hash;
  int chunk_index{0};
  std::string chunk_hash;
  std::size_t chunk_offset{0};
  int chunk_size{0};
};

struct FileDeletionPlan {
  int64_t file_id{0};
  std::size_t queued_chunk_count{0};
};

struct PendingChunkDeletion {
  std::string chunk_hash;
  std::string claim_token;
  int retry_count{0};
};

struct AuthUser {
  int64_t user_id{0};
  std::string username;
  UserRole role{UserRole::GUEST};
};

enum class AuthenticationStatus : uint8_t {
  AUTHENTICATED,
  INVALID_CREDENTIALS,
  STORAGE_ERROR,
};

struct AuthenticationResult {
  AuthenticationStatus status{AuthenticationStatus::INVALID_CREDENTIALS};
  std::optional<AuthUser> user;
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
