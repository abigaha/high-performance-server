#include "main_functions.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

constexpr char kAuthSecretEnv[] = "AUTH_SECRET";
constexpr char kConfigPath[] = "/tmp/hps_auth_config_test.json";

class ScopedAuthSecret {
public:
  explicit ScopedAuthSecret(std::optional<std::string> value) {
    if (const char* current = std::getenv(kAuthSecretEnv)) {
      previous_value_ = current;
    }
    set_value(value);
  }

  ~ScopedAuthSecret() {
    if (previous_value_) {
      static_cast<void>(::setenv(kAuthSecretEnv, previous_value_->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv(kAuthSecretEnv));
    }
  }

  ScopedAuthSecret(const ScopedAuthSecret&) = delete;
  ScopedAuthSecret& operator=(const ScopedAuthSecret&) = delete;

private:
  static void set_value(const std::optional<std::string>& value) {
    const int result = value ? ::setenv(kAuthSecretEnv, value->c_str(), 1) : ::unsetenv(kAuthSecretEnv);
    if (result != 0) {
      throw std::runtime_error("无法设置 AUTH_SECRET 测试环境变量");
    }
  }

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
  ScopedAuthSecret auth_secret(std::nullopt);
  TempConfigFile config(R"({"server":{"port":9090}})");

  EXPECT_THROW(load_test_config(config.path()), std::runtime_error);
}

TEST(ConfigTest, EnvironmentAuthSecretOverridesConfigFile) {
  ScopedAuthSecret auth_secret(std::string("environment-secret"));
  TempConfigFile config(R"({"server":{"auth_secret":"config-secret"}})");

  EXPECT_EQ(load_test_config(config.path()).auth_secret, "environment-secret");
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
