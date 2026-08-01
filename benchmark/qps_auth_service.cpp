#include "auth_service.h"
#include "database_pool.h"
#include "mock_connection.h"
#include "qps_runner.hpp"

#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

int run_benchmark() {
  const std::string password = "test_pass";
  const std::string salt = "salt";
  const std::string password_hash = hps::hash_password(password, salt);
  auto pool = std::make_unique<hps::DatabasePool>([password_hash, salt]() -> std::unique_ptr<hps::IConnection> {
    auto conn = std::make_unique<hps::MockConnection>();
    conn->query_result = hps::QueryResult{
      .columns = {"user_id", "username", "password_hash", "salt", "role", "email", "vip_expires_at", "created_at"},
      .rows = {{"1", "bench_user", password_hash, salt, "1", "bench@test.com", "", "2024-01-01 00:00:00.000000"}},
    };
    conn->execute_result = 1;
    conn->query_hook = [password_hash, salt](const std::string& sql,
                                             const std::vector<std::string>&) -> std::optional<hps::QueryResult> {
      if (sql.find("SELECT user_id, username, role") != std::string::npos) {
        return hps::QueryResult{
          .columns = {"user_id", "username", "role"},
          .rows = {{"1", "bench_user", "1"}},
        };
      }
      return hps::QueryResult{
        .columns = {"user_id", "username", "password_hash", "salt", "role", "email", "vip_expires_at", "created_at"},
        .rows = {{"1", "bench_user", password_hash, salt, "1", "bench@test.com", "", "2024-01-01 00:00:00.000000"}},
      };
    };
    return conn;
  });
  hps::DbConfig config;
  config.pool_size = 8;
  config.connect_timeout_ms = 1000;
  config.read_timeout_ms = 1000;
  if (!pool->init(config)) {
    return 1;
  }

  auto& pool_ref = *pool;
  auto auth = hps::create_auth_service(pool_ref, "bench-secret-key");
  auto& auth_ref = *auth;
  hps::AuthUser u;
  u.user_id = 1;
  u.username = "bench_user";
  u.role = hps::UserRole::NORMAL;
  auto token = auth_ref.generate_token(u);

  auto levels = hps::bench::default_qps_levels();

  hps::bench::run_qps_steps("DatabasePool CreateUser", levels, [&pool_ref](int tid) {
    hps::User new_user;
    new_user.username = "qps_user_" + std::to_string(tid);
    new_user.password_hash = "hash";
    new_user.salt = "salt";
    new_user.email = "qps@test.com";
    return pool_ref.create_user(new_user).status == hps::MutationStatus::OK;
  });

  hps::bench::run_qps_steps("Auth Authenticate", levels, [&auth_ref, &password](int) {
    const auto authenticated = auth_ref.authenticate("bench_user", password);
    return authenticated.status == hps::AuthenticationStatus::AUTHENTICATED && authenticated.user.has_value() &&
           authenticated.user->user_id == 1 && authenticated.user->role == hps::UserRole::NORMAL;
  });

  hps::bench::run_qps_steps("Auth ValidateToken", levels, [&token, &auth_ref](int) {
    const auto validated = auth_ref.validate_token(token);
    return validated.status == hps::TokenValidationStatus::AUTHENTICATED && validated.identity.user_id == 1 &&
           validated.identity.role == hps::UserRole::NORMAL;
  });

  pool->close();
  return 0;
}

} // namespace

int main() noexcept {
  try {
    return run_benchmark();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "认证服务 QPS 基准执行失败：%s\n", error.what());
  } catch (...) {
    std::fputs("认证服务 QPS 基准执行失败：未知异常\n", stderr);
  }
  return 1;
}
