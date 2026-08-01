#include "admin_bootstrap.h"
#include "auth_service.h"
#include "database_pool.h"
#include "main_functions.h"
#include "mock_connection.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hps {
namespace {

constexpr std::string_view kPassword = "correct-horse-battery-staple";

class ScopedEnvironment {
public:
  ScopedEnvironment(std::string name, std::optional<std::string> value) : name_(std::move(name)) {
    if (const char* current = std::getenv(name_.c_str())) {
      previous_ = current;
    }
    set(value);
  }

  ~ScopedEnvironment() noexcept { static_cast<void>(set(previous_)); }

  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
  bool set(const std::optional<std::string>& value) const noexcept {
    const int result = value ? ::setenv(name_.c_str(), value->c_str(), 1) : ::unsetenv(name_.c_str());
    return result == 0;
  }

  std::string name_;
  std::optional<std::string> previous_;
};

class TempConfigFile {
public:
  TempConfigFile() {
    std::array<char, 38> pattern{};
    constexpr std::string_view value = "/tmp/hps_admin_bootstrap_XXXXXX";
    std::ranges::copy(value, pattern.begin());
    const int descriptor = ::mkstemp(pattern.data());
    if (descriptor < 0) {
      throw std::runtime_error("无法创建唯一临时配置文件");
    }
    path_ = pattern.data();
    const std::string content = "{}";
    if (::write(descriptor, content.data(), content.size()) != static_cast<ssize_t>(content.size())) {
      static_cast<void>(::close(descriptor));
      static_cast<void>(::unlink(path_.c_str()));
      throw std::runtime_error("无法写入临时配置文件");
    }
    static_cast<void>(::close(descriptor));
  }

  ~TempConfigFile() { static_cast<void>(::unlink(path_.c_str())); }

  const std::string& path() const { return path_; }

  TempConfigFile(const TempConfigFile&) = delete;
  TempConfigFile& operator=(const TempConfigFile&) = delete;

private:
  std::string path_;
};

struct AdminDatabase {
  std::vector<User> users;
  std::vector<std::string> statements;
  std::vector<std::vector<std::string>> statement_params;
  int insert_count{0};
  int update_count{0};
  bool fail_queries{false};
  bool concurrent_admin_on_insert{false};
  bool concurrent_email_on_insert{false};
  std::unique_ptr<DatabasePool> pool;

  AdminDatabase() {
    pool = std::make_unique<DatabasePool>([this]() {
      auto connection = std::make_unique<MockConnection>();
      connection->query_hook = [this](const std::string& sql,
                                      const std::vector<std::string>& params) -> std::optional<QueryResult> {
        statements.push_back(sql);
        statement_params.push_back(params);
        if (fail_queries) {
          return std::nullopt;
        }
        auto match = users.end();
        if (sql.find("role = 3") != std::string::npos) {
          match = std::ranges::find_if(users, [](const User& user) { return user.role == UserRole::ADMIN; });
        } else if (sql.find("username = ?") != std::string::npos && !params.empty()) {
          match = std::ranges::find_if(users, [&params](const User& user) { return user.username == params[0]; });
        } else if (sql.find("email = ?") != std::string::npos && !params.empty()) {
          match = std::ranges::find_if(users, [&params](const User& user) { return user.email == params[0]; });
        }
        QueryResult result;
        if (match != users.end()) {
          result.rows.push_back(user_row(*match));
        }
        return result;
      };
      connection->execute_hook = [this](const std::string& sql,
                                        const std::vector<std::string>& params) -> std::optional<int64_t> {
        statements.push_back(sql);
        statement_params.push_back(params);
        if (sql.starts_with("INSERT INTO users")) {
          ++insert_count;
          if (concurrent_admin_on_insert) {
            users.push_back(make_user(99, "racing_admin", "racing@localhost.invalid", UserRole::ADMIN));
            return std::nullopt;
          }
          if (concurrent_email_on_insert) {
            users.push_back(make_user(100, "racing_user", params.at(3), UserRole::NORMAL));
            return std::nullopt;
          }
          User user;
          user.user_id = static_cast<int64_t>(users.size() + 1);
          user.username = params.at(0);
          user.password_hash = params.at(1);
          user.salt = params.at(2);
          user.role = UserRole::ADMIN;
          user.email = params.at(3);
          users.push_back(std::move(user));
          return 1;
        }
        if (sql.starts_with("UPDATE users")) {
          ++update_count;
          const auto id = std::stoll(params.at(3));
          auto user = std::ranges::find_if(users, [id](const User& candidate) { return candidate.user_id == id; });
          if (user == users.end()) {
            return 0;
          }
          user->email = params.at(0);
          user->password_hash = params.at(1);
          user->salt = params.at(2);
          user->vip_expires_at.reset();
          return 1;
        }
        return 0;
      };
      return connection;
    });
    DbConfig config;
    config.pool_size = 1;
    if (!pool->init(config)) {
      throw std::runtime_error("测试数据库池初始化失败");
    }
  }

  static User make_user(int64_t id, std::string username, std::string email, UserRole role) {
    User user;
    user.user_id = id;
    user.username = std::move(username);
    user.email = std::move(email);
    user.role = role;
    user.salt = "0123456789abcdef0123456789abcdef";
    user.password_hash = hash_password(std::string(kPassword), user.salt);
    user.created_at = "2026-07-27 00:00:00.000000";
    return user;
  }

  static std::vector<std::string> user_row(const User& user) {
    return {std::to_string(user.user_id),
            user.username,
            user.password_hash,
            user.salt,
            std::to_string(static_cast<int>(user.role)),
            user.email,
            user.vip_expires_at ? "2034-01-01 00:00:00.000000" : "",
            user.created_at};
  }
};

AdminBootstrapConfig enabled_config(std::string username = "admin_account",
                                    std::string password = std::string(kPassword),
                                    std::string email = "admin_account@localhost.invalid") {
  return {std::move(username), std::move(password), std::move(email)};
}

void expect_detail(const MutationResult<std::monostate>& result, MutationStatus status, std::string_view detail) {
  EXPECT_EQ(result.status, status);
  ASSERT_TRUE(result.detail.has_value());
  EXPECT_EQ(*result.detail, detail);
  EXPECT_EQ(result.detail->find(kPassword), std::string::npos);
}

TEST(AdminBootstrapTest, AllUnsetOrAllEmptyDisablesBootstrapWithoutDatabaseAccess) {
  AdminDatabase database;

  EXPECT_EQ(bootstrap_admin(*database.pool, {}).status, MutationStatus::OK);
  EXPECT_EQ(bootstrap_admin(*database.pool, {"", "", ""}).status, MutationStatus::OK);

  EXPECT_TRUE(database.statements.empty());
  EXPECT_EQ(database.insert_count, 0);
  EXPECT_EQ(database.update_count, 0);
}

TEST(AdminBootstrapTest, PartialOrEmptyConfigurationFailsClosed) {
  AdminDatabase database;
  const std::array<AdminBootstrapConfig, 4> invalid = {
    AdminBootstrapConfig{"admin_account", std::nullopt, "admin@localhost.invalid"},
    AdminBootstrapConfig{"admin_account", "", "admin@localhost.invalid"},
    AdminBootstrapConfig{std::nullopt, "", ""},
    AdminBootstrapConfig{"", "valid-password-123", ""},
  };

  for (const auto& config : invalid) {
    expect_detail(bootstrap_admin(*database.pool, config), MutationStatus::INVALID_STATE, "ADMIN_CONFIG_INCOMPLETE");
  }
  EXPECT_TRUE(database.statements.empty());
}

TEST(AdminBootstrapTest, RejectsInvalidUsernamePasswordAndEmail) {
  AdminDatabase database;

  expect_detail(bootstrap_admin(*database.pool, enabled_config("a")),
                MutationStatus::INVALID_STATE,
                "ADMIN_USERNAME_INVALID");
  expect_detail(bootstrap_admin(*database.pool, enabled_config(std::string(65, 'a'))),
                MutationStatus::INVALID_STATE,
                "ADMIN_USERNAME_INVALID");
  expect_detail(bootstrap_admin(*database.pool, enabled_config("admin_account", "fifteen-char-pw")),
                MutationStatus::INVALID_STATE,
                "ADMIN_PASSWORD_INVALID");
  expect_detail(bootstrap_admin(*database.pool, enabled_config("admin_account", std::string(kPassword), "bad-email")),
                MutationStatus::INVALID_STATE,
                "ADMIN_EMAIL_INVALID");
  expect_detail(
    bootstrap_admin(*database.pool,
                    enabled_config("admin_account", std::string(kPassword), std::string(117, 'a') + "@example.com")),
    MutationStatus::INVALID_STATE,
    "ADMIN_EMAIL_INVALID");
  EXPECT_TRUE(database.statements.empty());
}

TEST(AdminBootstrapTest, AcceptsValidEmailAt128CharacterBoundary) {
  AdminDatabase database;
  const std::string email = std::string(116, 'a') + "@example.com";
  ASSERT_EQ(email.size(), 128U);

  EXPECT_EQ(bootstrap_admin(*database.pool, enabled_config("admin_account", std::string(kPassword), email)).status,
            MutationStatus::OK);
}

TEST(AdminBootstrapTest, CreatesAdminOnlyWhenUsernameAndEmailAreFree) {
  AdminDatabase database;

  const auto result = bootstrap_admin(*database.pool, enabled_config());

  EXPECT_EQ(result.status, MutationStatus::OK);
  ASSERT_EQ(database.users.size(), 1U);
  EXPECT_EQ(database.users[0].role, UserRole::ADMIN);
  EXPECT_EQ(database.users[0].username, "admin_account");
  EXPECT_EQ(database.users[0].email, "admin_account@localhost.invalid");
  EXPECT_FALSE(database.users[0].vip_expires_at.has_value());
  EXPECT_NE(database.users[0].password_hash, kPassword);
  EXPECT_EQ(database.users[0].password_hash, hash_password(std::string(kPassword), database.users[0].salt));
  ASSERT_FALSE(database.statements.empty());
  EXPECT_NE(database.statements.front().find("role = 3"), std::string::npos);
}

TEST(AdminBootstrapTest, NewAndExistingAdminsAlwaysClearVipExpiry) {
  AdminDatabase new_database;

  ASSERT_EQ(bootstrap_admin(*new_database.pool, enabled_config()).status, MutationStatus::OK);
  ASSERT_EQ(new_database.users.size(), 1U);
  EXPECT_FALSE(new_database.users[0].vip_expires_at.has_value());
  const auto insert = std::ranges::find_if(new_database.statements,
                                           [](const std::string& sql) { return sql.starts_with("INSERT INTO users"); });
  ASSERT_NE(insert, new_database.statements.end());
  EXPECT_NE(insert->find("vip_expires_at"), std::string::npos);

  AdminDatabase existing_database;
  auto existing = AdminDatabase::make_user(7, "admin_account", "admin_account@localhost.invalid", UserRole::ADMIN);
  existing.vip_expires_at = std::chrono::system_clock::time_point{std::chrono::seconds{2'000'000'000}};
  existing_database.users.push_back(std::move(existing));

  ASSERT_EQ(bootstrap_admin(*existing_database.pool, enabled_config()).status, MutationStatus::OK);
  ASSERT_EQ(existing_database.users.size(), 1U);
  EXPECT_FALSE(existing_database.users[0].vip_expires_at.has_value());
  EXPECT_EQ(existing_database.update_count, 1);
  const auto update = std::ranges::find_if(existing_database.statements,
                                           [](const std::string& sql) { return sql.starts_with("UPDATE users"); });
  ASSERT_NE(update, existing_database.statements.end());
  EXPECT_NE(update->find("vip_expires_at = NULL"), std::string::npos);
}

TEST(AdminBootstrapTest, MatchingAdminIsIdempotentWhenVipExpiryIsAlreadyNullAndChangesAreRehashed) {
  AdminDatabase database;
  database.users.push_back(
    AdminDatabase::make_user(7, "admin_account", "admin_account@localhost.invalid", UserRole::ADMIN));

  EXPECT_EQ(bootstrap_admin(*database.pool, enabled_config()).status, MutationStatus::OK);
  EXPECT_EQ(database.update_count, 0);
  const auto old_salt = database.users[0].salt;

  const auto changed = enabled_config("admin_account", "a-different-secure-password", "new@localhost.invalid");
  EXPECT_EQ(bootstrap_admin(*database.pool, changed).status, MutationStatus::OK);
  EXPECT_EQ(database.update_count, 1);
  EXPECT_EQ(database.users[0].email, "new@localhost.invalid");
  EXPECT_NE(database.users[0].salt, old_salt);
  EXPECT_EQ(database.users[0].password_hash, hash_password("a-different-secure-password", database.users[0].salt));
}

TEST(AdminBootstrapTest, ExistingDifferentAdminFailsWithoutCreatingAnother) {
  AdminDatabase database;
  database.users.push_back(AdminDatabase::make_user(3, "first_admin", "first@localhost.invalid", UserRole::ADMIN));

  expect_detail(bootstrap_admin(*database.pool, enabled_config()), MutationStatus::CONFLICT, "ADMIN_ACCOUNT_CONFLICT");
  EXPECT_EQ(database.insert_count, 0);
  EXPECT_EQ(database.update_count, 0);
}

TEST(AdminBootstrapTest, NormalUsernameOrOccupiedEmailNeverPromotesAnAccount) {
  AdminDatabase username_database;
  username_database.users.push_back(
    AdminDatabase::make_user(4, "admin_account", "normal@localhost.invalid", UserRole::NORMAL));
  expect_detail(bootstrap_admin(*username_database.pool, enabled_config()),
                MutationStatus::CONFLICT,
                "ADMIN_USERNAME_CONFLICT");
  EXPECT_EQ(username_database.users[0].role, UserRole::NORMAL);
  EXPECT_EQ(username_database.update_count, 0);

  AdminDatabase email_database;
  email_database.users.push_back(
    AdminDatabase::make_user(5, "normal_account", "admin_account@localhost.invalid", UserRole::NORMAL));
  expect_detail(bootstrap_admin(*email_database.pool, enabled_config()),
                MutationStatus::CONFLICT,
                "ADMIN_EMAIL_CONFLICT");
  EXPECT_EQ(email_database.update_count, 0);
}

TEST(AdminBootstrapTest, ConcurrentUniqueConstraintConflictHasNonSensitiveDetail) {
  AdminDatabase database;
  database.concurrent_admin_on_insert = true;
  ASSERT_TRUE(database.concurrent_admin_on_insert);

  expect_detail(bootstrap_admin(*database.pool, enabled_config()), MutationStatus::CONFLICT, "ADMIN_SLOT_CONFLICT");
  EXPECT_EQ(database.insert_count, 1);
  EXPECT_EQ(database.update_count, 0);
}

TEST(AdminBootstrapTest, ConcurrentRegistrationWinningEmailReturnsNonSensitiveEmailConflict) {
  AdminDatabase database;
  database.concurrent_email_on_insert = true;
  ASSERT_TRUE(database.concurrent_email_on_insert);
  const auto config = enabled_config();

  const auto result = bootstrap_admin(*database.pool, config);

  expect_detail(result, MutationStatus::CONFLICT, "ADMIN_EMAIL_CONFLICT");
  EXPECT_EQ(result.detail->find(*config.email), std::string::npos);
  EXPECT_EQ(database.insert_count, 1);
  EXPECT_EQ(database.update_count, 0);
}

TEST(AdminBootstrapTest, StorageFailureIsStructuredAndDoesNotAttemptWrites) {
  AdminDatabase database;
  database.fail_queries = true;
  ASSERT_TRUE(database.fail_queries);

  expect_detail(bootstrap_admin(*database.pool, enabled_config()),
                MutationStatus::STORAGE_ERROR,
                "ADMIN_LOOKUP_FAILED");
  EXPECT_EQ(database.insert_count, 0);
  EXPECT_EQ(database.update_count, 0);
}

TEST(AdminBootstrapTest, DatabaseWritesUseParametersAndNeverEmbedCredentialsInSql) {
  AdminDatabase database;

  ASSERT_EQ(bootstrap_admin(*database.pool, enabled_config()).status, MutationStatus::OK);
  const auto insert = std::ranges::find_if(database.statements,
                                           [](const std::string& sql) { return sql.starts_with("INSERT INTO users"); });
  ASSERT_NE(insert, database.statements.end());
  EXPECT_EQ(insert->find(kPassword), std::string::npos);
  EXPECT_NE(insert->find("VALUES (?, ?, ?, 3, NULLIF(?, ''), NULL)"), std::string::npos);
}

TEST(AdminBootstrapTest, LoadConfigPreservesUnsetEmptyAndCompleteAdminEnvironment) {
  ScopedEnvironment auth_secret("AUTH_SECRET", "test-auth-secret");
  TempConfigFile config_file;
  std::array<std::string, 3> args = {"test_admin_bootstrap", "--config", config_file.path()};
  std::array<char*, 3> argv = {args[0].data(), args[1].data(), args[2].data()};

  {
    ScopedEnvironment username("ADMIN_USERNAME", std::nullopt);
    ScopedEnvironment password("ADMIN_PASSWORD", std::nullopt);
    ScopedEnvironment email("ADMIN_EMAIL", std::nullopt);
    const auto config = load_config(static_cast<int>(argv.size()), argv.data());
    EXPECT_FALSE(config.admin.username.has_value());
    EXPECT_FALSE(config.admin.password.has_value());
    EXPECT_FALSE(config.admin.email.has_value());
  }
  {
    ScopedEnvironment username("ADMIN_USERNAME", "");
    ScopedEnvironment password("ADMIN_PASSWORD", "");
    ScopedEnvironment email("ADMIN_EMAIL", "");
    const auto config = load_config(static_cast<int>(argv.size()), argv.data());
    EXPECT_EQ(config.admin.username, "");
    EXPECT_EQ(config.admin.password, "");
    EXPECT_EQ(config.admin.email, "");
  }
  {
    ScopedEnvironment username("ADMIN_USERNAME", "admin_account");
    ScopedEnvironment password("ADMIN_PASSWORD", std::string(kPassword));
    ScopedEnvironment email("ADMIN_EMAIL", "admin_account@localhost.invalid");
    const auto config = load_config(static_cast<int>(argv.size()), argv.data());
    EXPECT_EQ(config.admin.username, "admin_account");
    EXPECT_EQ(config.admin.password, kPassword);
    EXPECT_EQ(config.admin.email, "admin_account@localhost.invalid");
  }
}

} // namespace
} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
