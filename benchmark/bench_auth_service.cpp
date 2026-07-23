#include "auth_service.h"
#include "database_pool.h"
#include "mock_connection.h"

#include <benchmark/benchmark.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

struct AuthBenchFixture {
  hps::DatabasePool pool;
  std::unique_ptr<hps::IAuthService> auth;
  std::string test_token;

  AuthBenchFixture() :
      pool([]() -> std::unique_ptr<hps::IConnection> {
        auto conn = std::make_unique<hps::MockConnection>();
        conn->query_result = hps::QueryResult{
          .columns = {"user_id", "username", "password_hash", "salt", "role", "email", "created_at"},
          .rows = {{"1", "bench_user", "hash", "salt", "1", "bench@test.com", "2024-01-01"}},
        };
        conn->execute_result = 1;
        conn->query_hook = [](const std::string& sql,
                              const std::vector<std::string>& /*params*/) -> std::optional<hps::QueryResult> {
          if (sql.find("SELECT user_id, username, role") != std::string::npos) {
            return hps::QueryResult{
              .columns = {"user_id", "username", "role"},
              .rows = {{"1", "bench_user", "1"}},
            };
          }
          if (sql.find("SELECT user_id, username, password_hash") != std::string::npos) {
            return hps::QueryResult{
              .columns = {"user_id", "username", "password_hash", "salt", "role", "email", "created_at"},
              .rows = {{"1", "bench_user", "hash", "salt", "1", "bench@test.com", "2024-01-01"}},
            };
          }
          if (sql.find("SELECT 1 FROM users WHERE username") != std::string::npos) {
            return hps::QueryResult{
              .columns = {"exists"},
              .rows = {{"1"}},
            };
          }
          return hps::QueryResult{
            .columns = {"user_id", "username", "password_hash", "salt", "role", "email", "created_at"},
            .rows = {{"1", "bench_user", "hash", "salt", "1", "bench@test.com", "2024-01-01"}},
          };
        };
        return conn;
      }) {
    hps::DbConfig config;
    config.pool_size = 4;
    config.connect_timeout_ms = 1000;
    config.read_timeout_ms = 1000;
    pool.init(config);
    auth = hps::create_auth_service(pool, "bench-secret-key-for-testing");
    hps::AuthUser u;
    u.user_id = 1;
    u.username = "bench_user";
    u.role = hps::UserRole::NORMAL;
    test_token = auth->generate_token(u);
  }
};

} // namespace

static void BM_AuthRegister(benchmark::State& state) {
  AuthBenchFixture f;
  int count = state.range(0);
  hps::User u;
  u.password_hash = "hash";
  u.salt = "salt";
  u.email = "new@test.com";
  for (auto _ : state) {
    for (int i = 0; i < count; ++i) {
      u.username = "new_user_" + std::to_string(i);
      auto ok = f.pool.create_user(u);
      benchmark::DoNotOptimize(ok);
    }
  }
  state.SetItemsProcessed(state.iterations() * count);
}

BENCHMARK(BM_AuthRegister)->Arg(1000);

static void BM_AuthAuthenticate(benchmark::State& state) {
  AuthBenchFixture f;
  int count = state.range(0);
  for (auto _ : state) {
    for (int i = 0; i < count; ++i) {
      auto user = f.auth->authenticate("bench_user", "test_pass");
      benchmark::DoNotOptimize(user);
    }
  }
  state.SetItemsProcessed(state.iterations() * count);
}

BENCHMARK(BM_AuthAuthenticate)->Arg(1000);

static void BM_AuthValidateToken(benchmark::State& state) {
  AuthBenchFixture f;
  int count = state.range(0);
  for (auto _ : state) {
    for (int i = 0; i < count; ++i) {
      auto user = f.auth->validate_token(f.test_token);
      benchmark::DoNotOptimize(user);
    }
  }
  state.SetItemsProcessed(state.iterations() * count);
}

BENCHMARK(BM_AuthValidateToken)->Arg(1000);

BENCHMARK_MAIN();
