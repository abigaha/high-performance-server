#include "auth_service.h"
#include "database_pool.h"
#include "mock_connection.h"
#include "qps_runner.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

int main() {
  auto pool = std::make_unique<hps::DatabasePool>([]() -> std::unique_ptr<hps::IConnection> {
    auto conn = std::make_unique<hps::MockConnection>();
    conn->query_result = hps::QueryResult{
      .columns = {"user_id", "username", "password_hash", "salt", "role", "email", "created_at"},
      .rows = {{"1", "bench_user", "hash", "salt", "1", "bench@test.com", "2024-01-01"}},
    };
    conn->execute_result = 1;
    conn->query_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<hps::QueryResult> {
      if (sql.find("SELECT user_id, username, role") != std::string::npos) {
        return hps::QueryResult{
          .columns = {"user_id", "username", "role"},
          .rows = {{"1", "bench_user", "1"}},
        };
      }
      return hps::QueryResult{
        .columns = {"user_id", "username", "password_hash", "salt", "role", "email", "created_at"},
        .rows = {{"1", "bench_user", "hash", "salt", "1", "bench@test.com", "2024-01-01"}},
      };
    };
    return conn;
  });
  hps::DbConfig config;
  config.pool_size = 8;
  config.connect_timeout_ms = 1000;
  config.read_timeout_ms = 1000;
  pool->init(config);

  auto& pool_ref = *pool;
  auto auth = hps::create_auth_service(pool_ref, "bench-secret-key");
  auto& auth_ref = *auth;
  hps::AuthUser u;
  u.user_id = 1;
  u.username = "bench_user";
  u.role = hps::UserRole::NORMAL;
  auto token = auth_ref.generate_token(u);

  auto levels = hps::bench::default_qps_levels();

  hps::bench::run_qps_steps("Auth Register", levels, [&pool_ref](int tid) {
    hps::User new_user;
    new_user.username = "qps_user_" + std::to_string(tid);
    new_user.password_hash = "hash";
    new_user.salt = "salt";
    new_user.email = "qps@test.com";
    pool_ref.create_user(new_user);
  });

  hps::bench::run_qps_steps("Auth Authenticate", levels, [&auth_ref](int) {
    auth_ref.authenticate("bench_user", "test_pass");
  });

  hps::bench::run_qps_steps("Auth ValidateToken", levels, [&token, &auth_ref](int) { auth_ref.validate_token(token); });

  pool->close();
  return 0;
}
