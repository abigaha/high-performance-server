#include "database_pool.h"
#include "mock_connection.h"

#include <benchmark/benchmark.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

struct DbBenchFixture {
  hps::DatabasePool pool;

  DbBenchFixture() :
      pool([]() -> std::unique_ptr<hps::IConnection> {
        auto conn = std::make_unique<hps::MockConnection>();
        conn->query_result = hps::QueryResult{};
        conn->execute_result = 1;
        return conn;
      }) {
    hps::DbConfig config;
    config.pool_size = 4;
    config.connect_timeout_ms = 1000;
    config.read_timeout_ms = 1000;
    pool.init(config);
  }
};

} // namespace

static void BM_DatabasePool_GetUser(benchmark::State& state) {
  DbBenchFixture f;
  for (auto _ : state) {
    auto user = f.pool.get_user(1);
    benchmark::DoNotOptimize(user);
  }
}

BENCHMARK(BM_DatabasePool_GetUser);

static void BM_DatabasePool_CreateUser(benchmark::State& state) {
  DbBenchFixture f;
  hps::User u;
  u.username = "bench";
  u.password_hash = "hash";
  u.email = "bench@test.com";
  for (auto _ : state) {
    auto ok = f.pool.create_user(u);
    benchmark::DoNotOptimize(ok);
  }
}

BENCHMARK(BM_DatabasePool_CreateUser);

static void BM_DatabasePool_GetFileRecord(benchmark::State& state) {
  DbBenchFixture f;
  for (auto _ : state) {
    auto meta = f.pool.get_file_record_by_hash("abcdef1234567890abcdef1234567890");
    benchmark::DoNotOptimize(meta);
  }
}

BENCHMARK(BM_DatabasePool_GetFileRecord);

static void BM_DatabasePool_StoreFileRecord(benchmark::State& state) {
  DbBenchFixture f;
  hps::FileRecord record;
  record.file_name = "test.bin";
  record.file_hash = "abcdef1234567890abcdef1234567890";
  record.file_size = 1048576;
  for (auto _ : state) {
    auto ok = f.pool.store_file_record(record);
    benchmark::DoNotOptimize(ok);
  }
}

BENCHMARK(BM_DatabasePool_StoreFileRecord);

BENCHMARK_MAIN();
