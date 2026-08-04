#include "api_datetime.h"
#include "auth_service.h"
#include "database_pool.h"
#include "db_config.h"
#include "mock_connection.h"

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace hps;

namespace {

// ============ HMAC-SHA256 辅助工具 ============

std::string to_hex(const unsigned char* data, std::size_t len) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex(len * 2, '\0');
  for (std::size_t i = 0; i < len; ++i) {
    hex[i * 2] = kHex[data[i] >> 4U];
    hex[i * 2 + 1] = kHex[data[i] & 0x0FU];
  }
  return hex;
}

std::string hmac_sha256(const std::string& key, const std::string& data) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
  unsigned int hash_len = 0;
  HMAC(EVP_sha256(),
       key.data(),
       static_cast<int>(key.size()),
       reinterpret_cast<const unsigned char*>(data.data()),
       data.size(),
       hash.data(),
       &hash_len);
  return to_hex(hash.data(), hash_len);
}

std::string base64_encode(const std::string& in) {
  static constexpr std::string_view kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  auto remain = in.size();
  const auto* bytes = reinterpret_cast<const unsigned char*>(in.data());
  while (remain >= 3) {
    auto b0 = static_cast<unsigned int>(bytes[0]);
    auto b1 = static_cast<unsigned int>(bytes[1]);
    auto b2 = static_cast<unsigned int>(bytes[2]);
    out += kChars[b0 >> 2U];
    out += kChars[((b0 << 4U) | (b1 >> 4U)) & 0x3FU];
    out += kChars[((b1 << 2U) | (b2 >> 6U)) & 0x3FU];
    out += kChars[b2 & 0x3FU];
    bytes += 3;
    remain -= 3;
  }
  if (remain == 2) {
    auto b0 = static_cast<unsigned int>(bytes[0]);
    auto b1 = static_cast<unsigned int>(bytes[1]);
    out += kChars[b0 >> 2U];
    out += kChars[((b0 << 4U) | (b1 >> 4U)) & 0x3FU];
    out += kChars[(b1 << 2U) & 0x3FU];
    out += '=';
  } else if (remain == 1) {
    auto b0 = static_cast<unsigned int>(bytes[0]);
    out += kChars[b0 >> 2U];
    out += kChars[(b0 << 4U) & 0x3FU];
    out += "==";
  }
  return out;
}

std::string make_token(const std::string& secret, int64_t user_id, UserRole role, int64_t exp) {
  auto payload =
    "uid" + std::to_string(user_id) + "role" + std::to_string(static_cast<int>(role)) + "exp" + std::to_string(exp);
  auto payload_b64 = base64_encode(payload);
  auto sig = hmac_sha256(secret, payload_b64);
  return payload_b64 + "." + sig;
}

User make_user(int64_t user_id,
               std::string username,
               UserRole role,
               std::optional<std::chrono::system_clock::time_point> vip_expires_at = std::nullopt) {
  User user;
  user.user_id = user_id;
  user.username = std::move(username);
  user.role = role;
  user.vip_expires_at = vip_expires_at;
  return user;
}

// ============ MockDatabasePool ============

class MockDatabasePool : public IDatabasePool {
public:
  std::unordered_map<int64_t, User> users;
  std::unordered_map<std::string, AuthUser> auth_users;
  bool exists_flag = false;
  std::optional<LookupStatus> forced_user_status;
  std::optional<LookupStatus> forced_auth_user_status;
  LookupResult<FileRecord> file_detail_result{LookupStatus::STORAGE_ERROR, std::nullopt};
  int legacy_user_lookup_count = 0;
  int legacy_auth_user_lookup_count = 0;

  bool init(const DbConfig& /*config*/) override { return true; }

  void close() override {}

  LookupResult<User> get_user_result(int64_t user_id) override {
    if (forced_user_status) {
      return {*forced_user_status, std::nullopt};
    }
    auto it = users.find(user_id);
    if (it != users.end())
      return {LookupStatus::FOUND, it->second};
    return {LookupStatus::NOT_FOUND, std::nullopt};
  }

  std::optional<User> get_user(int64_t user_id) override {
    ++legacy_user_lookup_count;
    return IDatabasePool::get_user(user_id);
  }

  MutationResult<std::monostate> create_user(const User& user) override {
    if (users.find(user.user_id) != users.end())
      return {MutationStatus::CONFLICT, std::nullopt, "USERNAME_CONFLICT"};
    users[user.user_id] = user;
    return {MutationStatus::OK, std::monostate{}, std::nullopt};
  }

  MutationResult<std::monostate> update_user(const User& user) override {
    users[user.user_id] = user;
    return {MutationStatus::OK, std::monostate{}, std::nullopt};
  }

  bool username_exists(const std::string& username) override {
    return exists_flag || auth_users.find(username) != auth_users.end();
  }

  std::optional<int64_t> store_file_record(const FileRecord& /*record*/) override { return std::nullopt; }

  std::optional<FileRecord> get_file_record(int64_t /*file_id*/) override { return std::nullopt; }

  LookupResult<FileRecord> get_file_record_result(int64_t /*file_id*/) override { return file_detail_result; }

  std::optional<FileRecord> get_file_record_by_hash(const std::string& /*hash*/) override { return std::nullopt; }

  std::vector<FileRecord> search_files(const std::string& /*name_pattern*/, int /*offset*/, int /*limit*/) override {
    return {};
  }

  std::vector<FileRecord> search_files_ext(const std::string& /*name_pattern*/,
                                           const std::string& /*type_filter*/,
                                           int /*offset*/,
                                           int /*limit*/,
                                           int& /*out_total*/) override {
    return {};
  }

  bool update_file_record(const FileRecord& /*record*/) override { return false; }

  bool store_file_chunks(const std::vector<FileChunkRecord>& /*chunks*/) override { return false; }

  std::vector<FileChunkRecord> get_file_chunks(const std::string& /*file_hash*/) override { return {}; }

  bool chunk_exists(const std::string& /*chunk_hash*/) override { return false; }

  LookupResult<AuthUser> get_auth_user_result(const std::string& username) override {
    if (forced_auth_user_status) {
      return {*forced_auth_user_status, std::nullopt};
    }
    auto it = auth_users.find(username);
    if (it != auth_users.end())
      return {LookupStatus::FOUND, it->second};
    return {LookupStatus::NOT_FOUND, std::nullopt};
  }

  std::optional<AuthUser> get_auth_user(const std::string& username) override {
    ++legacy_auth_user_lookup_count;
    return IDatabasePool::get_auth_user(username);
  }

  bool verify_password(const std::string& /*username*/, const std::string& /*password*/) override { return false; }

  std::vector<MusicMeta> list_music_library(const std::string& /*search*/,
                                            int /*offset*/,
                                            int /*limit*/,
                                            int& /*out_total*/) override {
    return {};
  }

  std::optional<MusicMeta> get_music_meta(int64_t /*music_id*/) override { return std::nullopt; }

  std::optional<MusicMeta> get_music_by_file_id(int64_t /*file_id*/) override { return std::nullopt; }

  int64_t create_music_meta(const MusicMeta& /*meta*/) override { return 0; }

  bool update_music_meta(const MusicMeta& /*meta*/) override { return false; }

  bool delete_music_meta(int64_t /*music_id*/) override { return false; }

  MutationResult<std::vector<Playlist>> get_user_playlists(int64_t user_id, int64_t actor_id) override {
    (void)user_id;
    (void)actor_id;
    return {};
  }

  MutationResult<Playlist> create_playlist(const Playlist& playlist, int64_t actor_id) override {
    (void)playlist;
    (void)actor_id;
    return {};
  }

  MutationResult<Playlist> update_playlist(int64_t playlist_id,
                                           int64_t actor_id,
                                           const std::string& name,
                                           const std::string& description) override {
    (void)playlist_id;
    (void)actor_id;
    (void)name;
    (void)description;
    return {};
  }

  MutationResult<std::monostate> delete_playlist(int64_t playlist_id, int64_t actor_id) override {
    (void)playlist_id;
    (void)actor_id;
    return {};
  }

  MutationResult<std::vector<PlaylistItem>> get_playlist_items(int64_t playlist_id, int64_t actor_id) override {
    (void)playlist_id;
    (void)actor_id;
    return {};
  }

  MutationResult<std::monostate> add_playlist_item(int64_t playlist_id, int64_t actor_id, int64_t music_id) override {
    (void)playlist_id;
    (void)actor_id;
    (void)music_id;
    return {};
  }

  MutationResult<std::monostate> remove_playlist_item(int64_t playlist_id,
                                                      int64_t actor_id,
                                                      int64_t music_id) override {
    (void)playlist_id;
    (void)actor_id;
    (void)music_id;
    return {};
  }

  MutationResult<std::monostate> reorder_playlist_items(int64_t playlist_id,
                                                        int64_t actor_id,
                                                        const std::vector<int64_t>& music_ids) override {
    (void)playlist_id;
    (void)actor_id;
    (void)music_ids;
    return {};
  }
};

} // namespace

TEST(AuthServiceTimeTest, FormatsOnlyStrictMysqlUtcDateTimesAsRfc3339Utc) {
  EXPECT_EQ(format_api_datetime("2026-01-02 03:04:05"), std::optional<std::string>{"2026-01-02T03:04:05.000000Z"});
  EXPECT_EQ(format_api_datetime("2024-02-29 03:04:05.123456"),
            std::optional<std::string>{"2024-02-29T03:04:05.123456Z"});

  for (const std::string& invalid : {"",
                                     "0000-01-01 00:00:00",
                                     "2025-02-29 00:00:00",
                                     "2026-01-01 24:00:00",
                                     "2026-01-01 00:00:00+08:00",
                                     "2026-01-01T00:00:00Z"}) {
    EXPECT_FALSE(format_api_datetime(invalid).has_value()) << invalid;
  }
}

TEST(AuthServiceTest, FileDetailLookupCompatibilityDefaultIsStorageError) {
  class CompatibilityDatabase final : public MockDatabasePool {
  public:
    LookupResult<FileRecord> get_file_record_result(int64_t file_id) override {
      return MockDatabasePool::get_file_record_result(file_id);
    }
  } database;

  const auto result = database.get_file_record_result(7);

  EXPECT_EQ(result.status, LookupStatus::STORAGE_ERROR);
  EXPECT_FALSE(result.value.has_value());
}

TEST(AuthServiceTest, RegisterNewUser) {
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret");

  User new_user;
  new_user.user_id = 100;
  new_user.username = "newuser";
  new_user.password_hash = hash_password("pass123", "test_salt_16bytes!");
  new_user.salt = "test_salt_16bytes!";
  new_user.role = UserRole::NORMAL;
  ASSERT_EQ(mock_db.create_user(new_user).status, MutationStatus::OK);

  mock_db.auth_users["newuser"] = AuthUser{100, "newuser", UserRole::NORMAL};

  auto result = auth->authenticate("newuser", "pass123");
  ASSERT_EQ(result.status, AuthenticationStatus::AUTHENTICATED);
  ASSERT_TRUE(result.user.has_value());
  EXPECT_EQ(result.user->user_id, 100);
}

TEST(AuthServiceTest, AuthenticateValidCredentials) {
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret");

  auto salt = generate_salt();
  auto pwd_hash = hash_password("password123", salt);

  User user;
  user.user_id = 1;
  user.username = "testuser";
  user.password_hash = pwd_hash;
  user.salt = salt;
  user.role = UserRole::NORMAL;
  mock_db.users[1] = user;
  mock_db.auth_users["testuser"] = AuthUser{1, "testuser", UserRole::NORMAL};

  auto result = auth->authenticate("testuser", "password123");
  ASSERT_EQ(result.status, AuthenticationStatus::AUTHENTICATED);
  ASSERT_TRUE(result.user.has_value());
  EXPECT_EQ(result.user->user_id, 1);
  EXPECT_EQ(result.user->username, "testuser");
  EXPECT_EQ(result.user->role, UserRole::NORMAL);
  EXPECT_EQ(mock_db.legacy_auth_user_lookup_count, 0);
  EXPECT_EQ(mock_db.legacy_user_lookup_count, 0);
}

TEST(AuthServiceTest, AuthenticateInvalidPassword) {
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret");

  auto salt = generate_salt();
  auto pwd_hash = hash_password("correct_password", salt);

  User user;
  user.user_id = 2;
  user.username = "user2";
  user.password_hash = pwd_hash;
  user.salt = salt;
  mock_db.users[2] = user;
  mock_db.auth_users["user2"] = AuthUser{2, "user2", UserRole::NORMAL};

  auto result = auth->authenticate("user2", "wrong_password");
  EXPECT_EQ(result.status, AuthenticationStatus::INVALID_CREDENTIALS);
  EXPECT_FALSE(result.user.has_value());
}

TEST(AuthServiceTest, AuthenticateNonExistentUser) {
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret");

  auto result = auth->authenticate("nonexistent", "any_password");
  EXPECT_EQ(result.status, AuthenticationStatus::INVALID_CREDENTIALS);
  EXPECT_FALSE(result.user.has_value());
}

TEST(AuthServiceTest, AuthenticateReturnsStorageErrorForDatabaseFailuresAndInvalidData) {
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret");

  mock_db.forced_auth_user_status = LookupStatus::STORAGE_ERROR;
  EXPECT_EQ(auth->authenticate("testuser", "password").status, AuthenticationStatus::STORAGE_ERROR);

  mock_db.forced_auth_user_status = LookupStatus::INVALID_DATA;
  EXPECT_EQ(auth->authenticate("testuser", "password").status, AuthenticationStatus::STORAGE_ERROR);

  mock_db.forced_auth_user_status.reset();
  mock_db.auth_users["testuser"] = AuthUser{7, "testuser", UserRole::NORMAL};
  mock_db.forced_user_status = LookupStatus::STORAGE_ERROR;
  EXPECT_EQ(auth->authenticate("testuser", "password").status, AuthenticationStatus::STORAGE_ERROR);

  mock_db.forced_user_status = LookupStatus::INVALID_DATA;
  EXPECT_EQ(auth->authenticate("testuser", "password").status, AuthenticationStatus::STORAGE_ERROR);
  EXPECT_EQ(mock_db.legacy_auth_user_lookup_count, 0);
  EXPECT_EQ(mock_db.legacy_user_lookup_count, 0);
}

TEST(AuthServiceTest, ValidateTokenValid) {
  MockDatabasePool mock_db;
  constexpr std::time_t kNow = 2'000'000'000;
  auto auth = create_auth_service(mock_db, "test-secret", [] { return std::chrono::system_clock::from_time_t(kNow); });

  AuthUser user;
  user.user_id = 42;
  user.username = "valid_user";
  user.role = UserRole::VIP;
  auto stored_user = make_user(42, "valid_user", UserRole::VIP, std::chrono::system_clock::from_time_t(kNow + 3600));
  stored_user.email = "valid_user@example.com";
  stored_user.created_at = "2026-01-01 00:00:00.000000";
  mock_db.users[42] = std::move(stored_user);

  auto token = auth->generate_token(user);
  EXPECT_FALSE(token.empty());

  auto validated = auth->validate_token(token);
  EXPECT_EQ(validated.status, TokenValidationStatus::AUTHENTICATED);
  EXPECT_EQ(validated.identity.user_id, 42);
  EXPECT_EQ(validated.identity.role, UserRole::VIP);
  EXPECT_EQ(validated.identity.username, "valid_user");
  EXPECT_EQ(validated.identity.vip_status, VipStatus::ACTIVE);
  ASSERT_TRUE(validated.profile);
  EXPECT_EQ(validated.profile->user_id, 42);
  EXPECT_EQ(validated.profile->username, "valid_user");
  EXPECT_EQ(validated.profile->email, "valid_user@example.com");
  EXPECT_EQ(validated.profile->created_at, "2026-01-01 00:00:00.000000");
  EXPECT_EQ(mock_db.legacy_user_lookup_count, 0);
}

TEST(AuthServiceTest, GenerateTokenRejectsExpiryBeyondSystemClockRange) {
  MockDatabasePool mock_db;
  const auto now = std::chrono::system_clock::time_point::max() - std::chrono::hours{24} + std::chrono::seconds{1};
  auto auth = create_auth_service(mock_db, "test-secret", [now] { return now; });

  EXPECT_TRUE(auth->generate_token(AuthUser{42, "boundary", UserRole::NORMAL}).empty());
}

TEST(AuthServiceTest, ValidateTokenTampered) {
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret");

  AuthUser user;
  user.user_id = 10;
  user.username = "tamper_test";
  user.role = UserRole::NORMAL;

  auto token = auth->generate_token(user);
  ASSERT_FALSE(token.empty());

  // 篡改 payload 中的一个字符
  auto dot_pos = token.find('.');
  ASSERT_NE(dot_pos, std::string::npos);
  std::string tampered = token;
  tampered[dot_pos / 2] ^= 0x01;

  auto validated = auth->validate_token(tampered);
  EXPECT_EQ(validated.status, TokenValidationStatus::INVALID);
  EXPECT_EQ(validated.identity.role, UserRole::GUEST);
}

TEST(AuthServiceTest, ValidateTokenRejectsSignatureWithWrongLength) {
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret");
  AuthUser user{10, "signature-test", UserRole::NORMAL};
  auto token = auth->generate_token(user);
  ASSERT_FALSE(token.empty());
  token.pop_back();

  const auto result = auth->validate_token(token);

  EXPECT_EQ(result.status, TokenValidationStatus::INVALID);
}

TEST(AuthServiceTest, ValidateTokenExpired) {
  MockDatabasePool mock_db;
  std::string secret = "test-secret";
  auto auth = create_auth_service(mock_db, secret);

  // 构造过期 token（exp = 1000000000，远早于当前时间）
  auto expired_token = make_token(secret, 99, UserRole::NORMAL, 1000000000);

  auto validated = auth->validate_token(expired_token);
  EXPECT_EQ(validated.status, TokenValidationStatus::INVALID);
  EXPECT_EQ(validated.identity.role, UserRole::GUEST);
}

TEST(AuthServiceTest, ValidateTokenUsesCurrentDatabaseRoleInsteadOfTokenRole) {
  MockDatabasePool mock_db;
  constexpr std::time_t kNow = 2'000'000'000;
  auto auth = create_auth_service(mock_db, "test-secret", [] { return std::chrono::system_clock::from_time_t(kNow); });
  mock_db.users[7] = make_user(7, "changed", UserRole::NORMAL);

  auto token = make_token("test-secret", 7, UserRole::VIP, kNow + 60);
  auto result = auth->validate_token(token);

  EXPECT_EQ(result.status, TokenValidationStatus::AUTHENTICATED);
  EXPECT_EQ(result.identity.user_id, 7);
  EXPECT_EQ(result.identity.role, UserRole::NORMAL);
  EXPECT_EQ(result.identity.vip_status, VipStatus::NONE);
}

TEST(AuthServiceTest, ValidateTokenReflectsExpiredVipImmediately) {
  MockDatabasePool mock_db;
  constexpr std::time_t kNow = 2'000'000'000;
  auto auth = create_auth_service(mock_db, "test-secret", [] { return std::chrono::system_clock::from_time_t(kNow); });
  mock_db.users[8] = make_user(8, "expired", UserRole::VIP, std::chrono::system_clock::from_time_t(kNow));

  auto token = make_token("test-secret", 8, UserRole::VIP, kNow + 60);
  auto result = auth->validate_token(token);

  EXPECT_EQ(result.status, TokenValidationStatus::AUTHENTICATED);
  EXPECT_EQ(result.identity.role, UserRole::NORMAL);
  EXPECT_EQ(result.identity.vip_status, VipStatus::EXPIRED);
}

TEST(AuthServiceTest, ValidateTokenUsesSubsecondClockAtVipExpirationBoundary) {
  MockDatabasePool mock_db;
  const auto expires_at = std::chrono::system_clock::time_point{std::chrono::seconds{2'000'000'000}} +
                          std::chrono::microseconds{1} + std::chrono::microseconds{1};
  const auto now = expires_at + std::chrono::microseconds{1};
  auto auth = create_auth_service(mock_db, "test-secret", [now] { return now; });
  mock_db.users[8] = make_user(8, "subsecond-boundary", UserRole::VIP, expires_at);

  const auto result = auth->validate_token(make_token("test-secret", 8, UserRole::VIP, 2'000'000'060));

  EXPECT_EQ(result.status, TokenValidationStatus::AUTHENTICATED);
  EXPECT_EQ(result.identity.role, UserRole::NORMAL);
  EXPECT_EQ(result.identity.vip_status, VipStatus::EXPIRED);
}

TEST(AuthServiceTest, GenerateTokenRejectsSystemClockOverflow) {
  const auto now = std::chrono::system_clock::time_point::max() - std::chrono::hours{24} + std::chrono::seconds{1};
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret", [now] { return now; });

  EXPECT_TRUE(auth->generate_token(AuthUser{42, "boundary", UserRole::NORMAL}).empty());
}

TEST(AuthServiceTest, ValidateTokenReflectsAdministratorChangeImmediately) {
  MockDatabasePool mock_db;
  constexpr std::time_t kNow = 2'000'000'000;
  auto auth = create_auth_service(mock_db, "test-secret", [] { return std::chrono::system_clock::from_time_t(kNow); });
  mock_db.users[9] = make_user(9, "administrator", UserRole::ADMIN);

  auto token = make_token("test-secret", 9, UserRole::NORMAL, kNow + 60);
  auto result = auth->validate_token(token);

  EXPECT_EQ(result.status, TokenValidationStatus::AUTHENTICATED);
  EXPECT_EQ(result.identity.role, UserRole::ADMIN);
}

TEST(AuthServiceTest, ValidateTokenReturnsGuestWhenUserWasDeleted) {
  MockDatabasePool mock_db;
  constexpr std::time_t kNow = 2'000'000'000;
  auto auth = create_auth_service(mock_db, "test-secret", [] { return std::chrono::system_clock::from_time_t(kNow); });

  auto token = make_token("test-secret", 10, UserRole::ADMIN, kNow + 60);
  auto result = auth->validate_token(token);

  EXPECT_EQ(result.status, TokenValidationStatus::USER_NOT_FOUND);
  EXPECT_EQ(result.identity.role, UserRole::GUEST);
}

TEST(AuthServiceTest, ValidateTokenTreatsExpiryEqualToNowAsInvalid) {
  MockDatabasePool mock_db;
  constexpr std::time_t kNow = 2'000'000'000;
  auto auth = create_auth_service(mock_db, "test-secret", [] { return std::chrono::system_clock::from_time_t(kNow); });
  mock_db.users[11] = make_user(11, "boundary", UserRole::NORMAL);

  auto result = auth->validate_token(make_token("test-secret", 11, UserRole::VIP, kNow));

  EXPECT_EQ(result.status, TokenValidationStatus::INVALID);
  EXPECT_EQ(result.identity.role, UserRole::GUEST);
}

TEST(AuthServiceTest, ValidateTokenRejectsMalformedNumbersWithoutThrowing) {
  MockDatabasePool mock_db;
  constexpr std::time_t kNow = 2'000'000'000;
  auto auth = create_auth_service(mock_db, "test-secret", [] { return std::chrono::system_clock::from_time_t(kNow); });
  const auto payload = base64_encode("uidnot-a-numberrole2exp2000000060");
  const auto token = payload + "." + hmac_sha256("test-secret", payload);

  EXPECT_NO_THROW({
    const auto result = auth->validate_token(token);
    EXPECT_EQ(result.status, TokenValidationStatus::INVALID);
    EXPECT_EQ(result.identity.role, UserRole::GUEST);
  });
}

TEST(AuthServiceTest, ValidateTokenReturnsStorageErrorWhenDatabaseThrows) {
  MockDatabasePool mock_db;
  mock_db.forced_user_status = LookupStatus::STORAGE_ERROR;

  constexpr std::time_t kNow = 2'000'000'000;
  auto auth = create_auth_service(mock_db, "test-secret", [] { return std::chrono::system_clock::from_time_t(kNow); });

  EXPECT_NO_THROW({
    const auto result = auth->validate_token(make_token("test-secret", 12, UserRole::VIP, kNow + 60));
    EXPECT_EQ(result.status, TokenValidationStatus::STORAGE_ERROR);
    EXPECT_EQ(result.identity.role, UserRole::GUEST);
  });
  EXPECT_EQ(mock_db.legacy_user_lookup_count, 0);
}

TEST(AuthServiceTest, ValidateTokenMapsInvalidDatabaseDataToStorageError) {
  MockDatabasePool mock_db;
  mock_db.forced_user_status = LookupStatus::INVALID_DATA;
  constexpr std::time_t kNow = 2'000'000'000;
  auto auth = create_auth_service(mock_db, "test-secret", [] { return std::chrono::system_clock::from_time_t(kNow); });

  const auto result = auth->validate_token(make_token("test-secret", 12, UserRole::VIP, kNow + 60));

  EXPECT_EQ(result.status, TokenValidationStatus::STORAGE_ERROR);
  EXPECT_EQ(result.identity.role, UserRole::GUEST);
  EXPECT_EQ(mock_db.legacy_user_lookup_count, 0);
}

TEST(AuthServiceTest, RegisterDuplicateUser) {
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret");

  User user1;
  user1.user_id = 200;
  user1.username = "dupuser";
  user1.password_hash = "hash1";
  user1.salt = "salt1";
  ASSERT_EQ(mock_db.create_user(user1).status, MutationStatus::OK);

  User user2;
  user2.user_id = 200;
  user2.username = "dupuser";
  user2.password_hash = "hash2";
  user2.salt = "salt2";
  const auto created = mock_db.create_user(user2);
  EXPECT_EQ(created.status, MutationStatus::CONFLICT);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
