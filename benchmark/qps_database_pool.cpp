#include "database_pool.h"
#include "mock_connection.h"
#include "qps_runner.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

int main() noexcept {
  try {
    auto levels = hps::bench::default_qps_levels();

    {
      hps::DatabasePool pool([]() -> std::unique_ptr<hps::IConnection> {
        auto conn = std::make_unique<hps::MockConnection>();
        conn->query_result = hps::QueryResult{};
        conn->execute_result = 1;
        return conn;
      });
      hps::DbConfig config;
      config.pool_size = 4;
      config.connect_timeout_ms = 1000;
      config.read_timeout_ms = 1000;
      pool.init(config);

      hps::bench::run_qps_steps("DatabasePool get_user", levels, [&pool](int) {
        auto user = pool.get_user(1);
        (void)user;
      });
    }

    {
      hps::DatabasePool pool([]() -> std::unique_ptr<hps::IConnection> {
        auto conn = std::make_unique<hps::MockConnection>();
        conn->query_result = hps::QueryResult{};
        conn->execute_result = 1;
        return conn;
      });
      hps::DbConfig config;
      config.pool_size = 4;
      config.connect_timeout_ms = 1000;
      config.read_timeout_ms = 1000;
      pool.init(config);

      hps::User u;
      u.username = "bench";
      u.password_hash = "hash";
      u.email = "bench@test.com";

      hps::bench::run_qps_steps("DatabasePool create_user", levels, [&pool, &u](int) {
        auto ok = pool.create_user(u);
        (void)ok;
      });
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "数据库连接池 QPS 基准失败: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "数据库连接池 QPS 基准失败: 未知异常\n";
  }
  return EXIT_FAILURE;
}
