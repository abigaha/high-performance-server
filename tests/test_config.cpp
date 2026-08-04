#include "main_functions.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr char kConfigPath[] = "/tmp/hps_auth_config_test.json";

class ScopedEnvironment {
public:
  ScopedEnvironment(std::string name, std::optional<std::string> value) : name_(std::move(name)) {
    if (const char* current = std::getenv(name_.c_str())) {
      previous_value_ = current;
    }
    set_value(value);
  }

  ~ScopedEnvironment() {
    if (previous_value_) {
      static_cast<void>(::setenv(name_.c_str(), previous_value_->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv(name_.c_str()));
    }
  }

  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
  void set_value(const std::optional<std::string>& value) const {
    const int result = value ? ::setenv(name_.c_str(), value->c_str(), 1) : ::unsetenv(name_.c_str());
    if (result != 0) {
      throw std::runtime_error("无法设置测试环境变量: " + name_);
    }
  }

  std::string name_;
  std::optional<std::string> previous_value_;
};

class TempConfigFile {
public:
  explicit TempConfigFile(const std::string& content) : path_(kConfigPath) {
    std::ofstream file(path_);
    if (!file.is_open()) {
      throw std::runtime_error("无法创建临时配置文件");
    }
    file << content;
    if (!file) {
      throw std::runtime_error("无法写入临时配置文件");
    }
  }

  ~TempConfigFile() { static_cast<void>(std::remove(path_.c_str())); }

  const std::string& path() const { return path_; }

  TempConfigFile(const TempConfigFile&) = delete;
  TempConfigFile& operator=(const TempConfigFile&) = delete;

private:
  std::string path_;
};

hps::ServerConfig load_test_config(const std::string& config_path) {
  std::array<std::string, 3> args = {"test_config", "--config", config_path};
  std::array<char*, 3> argv{};
  for (std::size_t i = 0; i < args.size(); ++i) {
    argv[i] = args[i].data();
  }
  return hps::load_config(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST(ConfigTest, MissingAuthSecretIsRejected) {
  ScopedEnvironment auth_secret("AUTH_SECRET", std::nullopt);
  TempConfigFile config(R"({"server":{"port":9090}})");

  EXPECT_THROW(load_test_config(config.path()), std::runtime_error);
}

TEST(ConfigTest, EnvironmentAuthSecretOverridesConfigFile) {
  ScopedEnvironment auth_secret("AUTH_SECRET", std::string("environment-secret"));
  TempConfigFile config(R"({"server":{"auth_secret":"config-secret"}})");

  EXPECT_EQ(load_test_config(config.path()).auth_secret, "environment-secret");
}

TEST(ConfigTest, DeploymentEnvironmentOverridesDatabaseConfig) {
  ScopedEnvironment auth_secret("AUTH_SECRET", std::string("environment-secret"));
  ScopedEnvironment db_host("DB_HOST", std::string("mysql"));
  ScopedEnvironment db_port("DB_PORT", std::string("3307"));
  ScopedEnvironment db_user("DB_USER", std::string("deployed-user"));
  ScopedEnvironment db_password("DB_PASSWORD", std::string("deployed-password"));
  ScopedEnvironment db_name("DB_NAME", std::string("deployed-database"));
  ScopedEnvironment server_port("SERVER_PORT", std::string("9191"));
  ScopedEnvironment server_thread_count("SERVER_THREAD_COUNT", std::string("24"));
  ScopedEnvironment server_backlog("SERVER_BACKLOG", std::string("1024"));
  ScopedEnvironment db_pool_size("DB_POOL_SIZE", std::string("48"));
  TempConfigFile config(R"({
    "server": {
      "thread_count": 4,
      "backlog": 128
    },
    "database": {
      "host": "config-host",
      "port": 3306,
      "username": "config-user",
      "password": "config-password",
      "database": "config-database",
      "pool_size": 10
    }
  })");

  const auto loaded = load_test_config(config.path());

  EXPECT_EQ(loaded.db.host, "mysql");
  EXPECT_EQ(loaded.db.port, 3307);
  EXPECT_EQ(loaded.db.username, "deployed-user");
  EXPECT_EQ(loaded.db.password, "deployed-password");
  EXPECT_EQ(loaded.db.database, "deployed-database");
  EXPECT_EQ(loaded.port, 9191);
  EXPECT_EQ(loaded.thread_count, 24U);
  EXPECT_EQ(loaded.backlog, 1024U);
  EXPECT_EQ(loaded.db.pool_size, 48U);
  EXPECT_EQ(loaded.db_host, loaded.db.host);
  EXPECT_EQ(loaded.db_port, loaded.db.port);
  EXPECT_EQ(loaded.db_user, loaded.db.username);
  EXPECT_EQ(loaded.db_password, loaded.db.password);
  EXPECT_EQ(loaded.db_name, loaded.db.database);
}

TEST(ConfigTest, NonPositiveEpollTimeoutIsClamped) {
  ScopedEnvironment auth_secret("AUTH_SECRET", std::nullopt);

  for (const int invalid_timeout : {0, -1}) {
    const std::string content =
      R"({"server":{"auth_secret":"config-secret","epoll_timeout_ms":)" + std::to_string(invalid_timeout) + "}}";
    TempConfigFile config(content);

    EXPECT_EQ(load_test_config(config.path()).epoll_timeout_ms, 1);
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
