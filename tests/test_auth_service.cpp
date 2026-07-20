#include "auth_service.h"
#include "database_pool.h"
#include "db_config.h"
#include "mock_connection.h"

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
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

// ============ MockDatabasePool ============

class MockDatabasePool : public IDatabasePool {
public:
  std::unordered_map<int64_t, User> users;
  std::unordered_map<std::string, AuthUser> auth_users;
  bool exists_flag = false;

  bool init(const DbConfig& /*config*/) override { return true; }

  void close() override {}

  std::optional<User> get_user(int64_t user_id) override {
    auto it = users.find(user_id);
    if (it != users.end())
      return it->second;
    return std::nullopt;
  }

  bool create_user(const User& user) override {
    if (users.find(user.user_id) != users.end())
      return false;
    users[user.user_id] = user;
    return true;
  }

  bool update_user(const User& user) override {
    users[user.user_id] = user;
    return true;
  }

  bool username_exists(const std::string& username) override {
    return exists_flag || auth_users.find(username) != auth_users.end();
  }

  std::optional<int64_t> store_file_record(const FileRecord& /*record*/) override { return std::nullopt; }

  std::optional<FileRecord> get_file_record(int64_t /*file_id*/) override { return std::nullopt; }

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

  bool delete_file_record(int64_t /*file_id*/) override { return false; }

  bool update_file_record(const FileRecord& /*record*/) override { return false; }

  bool store_file_chunks(const std::vector<FileChunkRecord>& /*chunks*/) override { return false; }

  std::vector<FileChunkRecord> get_file_chunks(const std::string& /*file_hash*/) override { return {}; }

  bool chunk_exists(const std::string& /*chunk_hash*/) override { return false; }

  std::optional<AuthUser> get_auth_user(const std::string& username) override {
    auto it = auth_users.find(username);
    if (it != auth_users.end())
      return it->second;
    return std::nullopt;
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

  std::vector<Playlist> get_user_playlists(int64_t /*user_id*/) override { return {}; }

  int64_t create_playlist(const Playlist& /*pl*/) override { return 0; }

  bool delete_playlist(int64_t /*playlist_id*/) override { return false; }

  std::vector<PlaylistItem> get_playlist_items(int64_t /*playlist_id*/) override { return {}; }

  bool add_playlist_item(int64_t /*playlist_id*/, int64_t /*music_id*/) override { return false; }

  bool remove_playlist_item(int64_t /*playlist_id*/, int64_t /*music_id*/) override { return false; }

  bool reorder_playlist_items(int64_t /*playlist_id*/, const std::vector<int64_t>& /*music_ids*/) override {
    return false;
  }
};

} // namespace

TEST(AuthServiceTest, RegisterNewUser) {
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret");

  User new_user;
  new_user.user_id = 100;
  new_user.username = "newuser";
  new_user.password_hash = hash_password("pass123", "test_salt_16bytes!");
  new_user.salt = "test_salt_16bytes!";
  new_user.role = UserRole::NORMAL;
  ASSERT_TRUE(mock_db.create_user(new_user));

  mock_db.auth_users["newuser"] = AuthUser{100, "newuser", UserRole::NORMAL};

  auto result = auth->authenticate("newuser", "pass123");
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->user_id, 100);
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
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->user_id, 1);
  EXPECT_EQ(result->username, "testuser");
  EXPECT_EQ(result->role, UserRole::NORMAL);
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
  EXPECT_FALSE(result.has_value());
}

TEST(AuthServiceTest, AuthenticateNonExistentUser) {
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret");

  auto result = auth->authenticate("nonexistent", "any_password");
  EXPECT_FALSE(result.has_value());
}

TEST(AuthServiceTest, ValidateTokenValid) {
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret");

  AuthUser user;
  user.user_id = 42;
  user.username = "valid_user";
  user.role = UserRole::VIP;

  auto token = auth->generate_token(user);
  EXPECT_FALSE(token.empty());

  auto validated = auth->validate_token(token);
  EXPECT_EQ(validated.user_id, 42);
  EXPECT_EQ(validated.role, UserRole::VIP);
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
  EXPECT_EQ(validated.user_id, 0);
  EXPECT_EQ(validated.role, UserRole::GUEST);
}

TEST(AuthServiceTest, ValidateTokenExpired) {
  MockDatabasePool mock_db;
  std::string secret = "test-secret";
  auto auth = create_auth_service(mock_db, secret);

  // 构造过期 token（exp = 1000000000，远早于当前时间）
  auto expired_token = make_token(secret, 99, UserRole::NORMAL, 1000000000);

  auto validated = auth->validate_token(expired_token);
  EXPECT_EQ(validated.user_id, 0);
  EXPECT_EQ(validated.role, UserRole::GUEST);
}

TEST(AuthServiceTest, RegisterDuplicateUser) {
  MockDatabasePool mock_db;
  auto auth = create_auth_service(mock_db, "test-secret");

  User user1;
  user1.user_id = 200;
  user1.username = "dupuser";
  user1.password_hash = "hash1";
  user1.salt = "salt1";
  ASSERT_TRUE(mock_db.create_user(user1));

  User user2;
  user2.user_id = 200;
  user2.username = "dupuser";
  user2.password_hash = "hash2";
  user2.salt = "salt2";
  bool created = mock_db.create_user(user2);
  EXPECT_FALSE(created);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
