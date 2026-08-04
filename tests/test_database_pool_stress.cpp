#include "database_pool.h"
#include "db_config.h"
#include "iconnection.h"
#include "mock_connection.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace hps;

namespace {

struct MockPool {
  std::vector<MockConnection*> connections;
  std::unique_ptr<DatabasePool> pool;
  int create_count = 0;

  explicit MockPool(std::size_t pool_size = 1, uint32_t timeout_ms = 3000) {
    auto factory = [this, pool_size]() -> std::unique_ptr<IConnection> {
      auto mc = std::make_unique<MockConnection>();
      connections.push_back(mc.get());
      ++create_count;
      return mc;
    };
    pool = std::make_unique<DatabasePool>(std::move(factory));
    DbConfig cfg;
    cfg.pool_size = pool_size;
    cfg.connect_timeout_ms = timeout_ms;
    pool->init(cfg);
  }
};

} // namespace

TEST(DatabasePoolStressTest, MaxPoolSize) {
  MockPool mp(3);
  ASSERT_GE(mp.connections.size(), 3U);

  for (auto* conn : mp.connections) {
    conn->query_result = QueryResult{};
  }

  auto u1 = mp.pool->get_user(1);
  auto u2 = mp.pool->get_user(2);
  auto u3 = mp.pool->get_user(3);

  EXPECT_FALSE(u1.has_value());
  EXPECT_FALSE(u2.has_value());
  EXPECT_FALSE(u3.has_value());
}

TEST(DatabasePoolStressTest, PoolExhaustionTimeout) {
  std::promise<void> hold_started;
  std::promise<void> release_hold;

  auto factory = [&]() -> std::unique_ptr<IConnection> {
    auto mc = std::make_unique<MockConnection>();
    mc->query_hook = [&](const std::string&, const std::vector<std::string>&) -> std::optional<QueryResult> {
      hold_started.set_value();
      release_hold.get_future().wait();
      return QueryResult{};
    };
    return mc;
  };

  DatabasePool pool(std::move(factory));
  DbConfig cfg;
  cfg.pool_size = 1;
  cfg.connect_timeout_ms = 200;
  ASSERT_TRUE(pool.init(cfg));

  auto fut1 = std::async(std::launch::async, [&]() { return pool.get_user(1); });
  hold_started.get_future().wait();

  auto fut2 = std::async(std::launch::async, [&]() { return pool.get_user(2); });
  auto start = std::chrono::steady_clock::now();
  auto result2 = fut2.get();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

  EXPECT_FALSE(result2.has_value());
  EXPECT_GE(elapsed.count(), 150);

  release_hold.set_value();
  fut1.get();
}

TEST(DatabasePoolStressTest, ReturnConnectionToPool) {
  MockPool mp(1);
  mp.connections[0]->query_result = QueryResult{};

  EXPECT_FALSE(mp.pool->get_user(1).has_value());
  EXPECT_FALSE(mp.pool->get_user(2).has_value());
  EXPECT_FALSE(mp.pool->get_user(3).has_value());
}

TEST(DatabasePoolStressTest, BrokenConnection) {
  auto factory = [&]() -> std::unique_ptr<IConnection> {
    auto mc = std::make_unique<MockConnection>();
    mc->is_open_result = false;
    mc->connect_result = true;
    mc->query_hook = [](const std::string&, const std::vector<std::string>&) -> std::optional<QueryResult> {
      return std::nullopt;
    };
    return mc;
  };

  DatabasePool pool(std::move(factory));
  DbConfig cfg;
  cfg.pool_size = 1;
  cfg.connect_timeout_ms = 100;
  ASSERT_TRUE(pool.init(cfg));

  auto result = pool.get_user(1);
  EXPECT_FALSE(result.has_value());
}

TEST(DatabasePoolStressTest, ConcurrentGetConnections) {
  MockPool mp(4, 5000);
  for (auto* conn : mp.connections) {
    conn->query_result = QueryResult{};
  }

  constexpr int kThreads = 8;
  std::atomic<int> success_count{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < 5; ++i) {
        auto user = mp.pool->get_user(t * 100 + i);
        if (!user.has_value()) {
          success_count.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  EXPECT_EQ(success_count.load(), kThreads * 5);
}

TEST(DatabasePoolStressTest, ReusesIdleConnectionsWithoutPing) {
  auto factory = [&]() -> std::unique_ptr<IConnection> {
    auto mc = std::make_unique<MockConnection>();
    mc->connect_result = true;
    mc->ping_result = true;
    mc->query_hook = [](const std::string&, const std::vector<std::string>&) -> std::optional<QueryResult> {
      return QueryResult{};
    };
    return mc;
  };

  DatabasePool pool(std::move(factory));
  DbConfig cfg;
  cfg.pool_size = 3;
  ASSERT_TRUE(pool.init(cfg));

  auto u1 = pool.get_user(1);
  auto u2 = pool.get_user(2);
  auto u3 = pool.get_user(3);

  EXPECT_FALSE(u1.has_value());
  EXPECT_FALSE(u2.has_value());
  EXPECT_FALSE(u3.has_value());
}

TEST(DatabasePoolStressTest, CloseWithActiveConnections) {
  MockPool mp(2);
  for (auto* conn : mp.connections) {
    conn->query_result = QueryResult{};
  }

  auto u1 = mp.pool->get_user(1);
  EXPECT_FALSE(u1.has_value());

  EXPECT_NO_THROW(mp.pool->close());
}

TEST(DatabasePoolStressTest, GetConnectionAfterClose) {
  MockPool mp(2);
  mp.pool->close();

  for (auto* conn : mp.connections) {
    conn->query_result = QueryResult{};
  }

  auto result = mp.pool->get_user(1);
  EXPECT_FALSE(result.has_value());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
