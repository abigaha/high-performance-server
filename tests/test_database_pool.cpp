#include "database_pool.h"
#include "db_config.h"
#include "iconnection.h"
#include "mock_connection.h"
#include "models.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hps {

// 工厂辅助：创建使用 MockConnection 的 DatabasePool
struct MockPool {
  std::vector<MockConnection*> connections;
  std::unique_ptr<DatabasePool> pool;
  int create_count = 0;

  explicit MockPool(std::size_t pool_size = 1) {
    auto factory = [this, pool_size]() -> std::unique_ptr<IConnection> {
      auto mc = std::make_unique<MockConnection>();
      connections.push_back(mc.get());
      ++create_count;
      return mc;
    };
    pool = std::make_unique<DatabasePool>(std::move(factory));
    DbConfig cfg;
    cfg.pool_size = pool_size;
    cfg.connect_timeout_ms = 500;
    pool->init(cfg);
  }
};

// ============================================================
// T1: DbConfig 默认值
// ============================================================
TEST(DatabaseModelTest, DbConfigDefaultValues) {
  DbConfig cfg;
  EXPECT_EQ(cfg.host, "127.0.0.1");
  EXPECT_EQ(cfg.port, 3306);
  EXPECT_EQ(cfg.username, "root");
  EXPECT_TRUE(cfg.password.empty());
  EXPECT_EQ(cfg.database, "music_server");
  EXPECT_EQ(cfg.pool_size, 10U);
  EXPECT_EQ(cfg.connect_timeout_ms, 3000U);
  EXPECT_EQ(cfg.read_timeout_ms, 5000U);
}

// ============================================================
// T2: User 模型字段
// ============================================================
TEST(DatabaseModelTest, UserModelFields) {
  User u;
  EXPECT_EQ(u.user_id, 0);
  EXPECT_TRUE(u.username.empty());
  EXPECT_TRUE(u.password_hash.empty());
  EXPECT_TRUE(u.email.empty());
  EXPECT_TRUE(u.created_at.empty());

  u.user_id = 42;
  u.username = "alice";
  u.password_hash = "abc123";
  u.email = "alice@test.com";
  u.created_at = "2024-01-01";

  EXPECT_EQ(u.user_id, 42);
  EXPECT_EQ(u.username, "alice");
  EXPECT_EQ(u.password_hash, "abc123");
  EXPECT_EQ(u.email, "alice@test.com");
  EXPECT_EQ(u.created_at, "2024-01-01");
}

// ============================================================
// T3: DownloadLog 模型字段
// ============================================================
TEST(DatabaseModelTest, DownloadLogModelFields) {
  DownloadLog dl;
  EXPECT_EQ(dl.log_id, 0);
  EXPECT_EQ(dl.user_id, 0);
  EXPECT_TRUE(dl.file_hash.empty());
  EXPECT_TRUE(dl.downloaded_at.empty());

  dl.log_id = 100;
  dl.user_id = 42;
  dl.file_hash = "abcdef";
  dl.downloaded_at = "2024-06-15";

  EXPECT_EQ(dl.log_id, 100);
  EXPECT_EQ(dl.user_id, 42);
  EXPECT_EQ(dl.file_hash, "abcdef");
  EXPECT_EQ(dl.downloaded_at, "2024-06-15");
}

// ============================================================
// T4: FileMeta 模型字段
// ============================================================
TEST(DatabaseModelTest, FileMetaModelFields) {
  FileMeta fm;
  EXPECT_TRUE(fm.file_hash.empty());
  EXPECT_TRUE(fm.file_path.empty());
  EXPECT_EQ(fm.file_size, 0U);
  EXPECT_TRUE(fm.content_type.empty());
  EXPECT_TRUE(fm.created_at.empty());

  fm.file_hash = "deadbeef";
  fm.file_path = "/music/song.mp3";
  fm.file_size = 1024000;
  fm.content_type = "audio/mpeg";
  fm.created_at = "2024-07-01";

  EXPECT_EQ(fm.file_hash, "deadbeef");
  EXPECT_EQ(fm.file_path, "/music/song.mp3");
  EXPECT_EQ(fm.file_size, 1024000U);
  EXPECT_EQ(fm.content_type, "audio/mpeg");
  EXPECT_EQ(fm.created_at, "2024-07-01");
}

// ============================================================
// T5: 连接池创建销毁
// ============================================================
TEST(DatabasePoolTest, PoolCreateDestroy) {
  int create_count = 0;
  auto factory = [&]() -> std::unique_ptr<IConnection> {
    ++create_count;
    auto mc = std::make_unique<MockConnection>();
    mc->connect_result = true;
    return mc;
  };

  DatabasePool pool(factory);
  DbConfig cfg;
  cfg.pool_size = 3;
  ASSERT_TRUE(pool.init(cfg));
  EXPECT_EQ(create_count, 3);

  // close 不应抛异常
  EXPECT_NO_THROW(pool.close());
}

// ============================================================
// T6: 连接获取归还
// ============================================================
TEST(DatabasePoolTest, ConnectionAcquireRelease) {
  MockPool mp(2);
  ASSERT_GE(mp.connections.size(), 2U);

  // 第一次查询：占一个连接
  mp.connections[0]->query_result = QueryResult{};
  mp.connections[1]->query_result = QueryResult{};

  // 连续两次 get_user（每次获取+归还）
  auto u1 = mp.pool->get_user(1);
  auto u2 = mp.pool->get_user(2);

  // 即使只有 2 个连接，也应该成功
  EXPECT_FALSE(u1.has_value()); // 查询结果为空
  EXPECT_FALSE(u2.has_value());
}

// ============================================================
// T7: 连接超时
// ============================================================
TEST(DatabasePoolTest, ConnectionTimeout) {
  // 在 query 中阻塞以持有连接
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

  DatabasePool pool(factory);
  DbConfig cfg;
  cfg.pool_size = 1;
  cfg.connect_timeout_ms = 200; // 200ms 超时
  ASSERT_TRUE(pool.init(cfg));

  // 线程 1：占用唯一连接
  auto fut1 = std::async(std::launch::async, [&]() { return pool.get_user(1); });

  // 等待线程 1 进入 query hook
  hold_started.get_future().wait();

  // 线程 2：应超时
  auto fut2 = std::async(std::launch::async, [&]() { return pool.get_user(2); });

  auto start = std::chrono::steady_clock::now();
  auto result2 = fut2.get();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

  EXPECT_FALSE(result2.has_value());
  EXPECT_GE(elapsed.count(), 150);
  EXPECT_LE(elapsed.count(), 1000);

  // 释放线程 1
  release_hold.set_value();
  auto result1 = fut1.get();
  EXPECT_FALSE(result1.has_value()); // 空结果
}

// ============================================================
// T8: get_user
// ============================================================
TEST(DatabasePoolTest, GetUser) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {"user_id", "username", "password_hash", "email", "created_at"};
  qr.rows.push_back({"42", "bob", "hash123", "bob@test.com", "2024-01-15"});
  mp.connections[0]->query_result = std::move(qr);

  auto user = mp.pool->get_user(42);
  ASSERT_TRUE(user.has_value());
  EXPECT_EQ(user->user_id, 42);
  EXPECT_EQ(user->username, "bob");
  EXPECT_EQ(user->password_hash, "hash123");
  EXPECT_EQ(user->email, "bob@test.com");
  EXPECT_EQ(user->created_at, "2024-01-15");
}

// ============================================================
// T9: create_user
// ============================================================
TEST(DatabasePoolTest, CreateUser) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1; // affected_rows

  User u;
  u.username = "carol";
  u.password_hash = "secure_hash";
  u.email = "carol@test.com";

  EXPECT_TRUE(mp.pool->create_user(u));

  // 验证 SQL 中含有参数化占位符
  EXPECT_NE(mp.connections[0]->last_sql.find('?'), std::string::npos);
  ASSERT_EQ(mp.connections[0]->last_params.size(), 3U);
  EXPECT_EQ(mp.connections[0]->last_params[0], "carol");
  EXPECT_EQ(mp.connections[0]->last_params[1], "secure_hash");
  EXPECT_EQ(mp.connections[0]->last_params[2], "carol@test.com");
}

TEST(DatabasePoolTest, CreateUserFails) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 0; // 没有行受影响

  User u;
  u.username = "dave";
  u.password_hash = "hash";
  u.email = "";

  EXPECT_FALSE(mp.pool->create_user(u));
}

// ============================================================
// T10: log_download + get_download_history
// ============================================================
TEST(DatabasePoolTest, DownloadLogLifecycle) {
  MockPool mp(1);

  // log_download
  mp.connections[0]->execute_result = 1;

  DownloadLog dl;
  dl.user_id = 42;
  dl.file_hash = "songhash123";
  EXPECT_TRUE(mp.pool->log_download(dl));

  // get_download_history
  QueryResult qr;
  qr.columns = {"log_id", "user_id", "file_hash", "downloaded_at"};
  qr.rows.push_back({"1", "42", "songhash123", "2024-06-15 10:00:00"});
  mp.connections[0]->query_result = std::move(qr);

  auto history = mp.pool->get_download_history(42);
  ASSERT_EQ(history.size(), 1U);
  EXPECT_EQ(history[0].log_id, 1);
  EXPECT_EQ(history[0].user_id, 42);
  EXPECT_EQ(history[0].file_hash, "songhash123");
  EXPECT_EQ(history[0].downloaded_at, "2024-06-15 10:00:00");
}

TEST(DatabasePoolTest, DownloadHistoryEmpty) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {"log_id", "user_id", "file_hash", "downloaded_at"};
  // 空行
  mp.connections[0]->query_result = std::move(qr);

  auto history = mp.pool->get_download_history(999);
  EXPECT_TRUE(history.empty());
}

// ============================================================
// T11: store_file_meta + get_file_meta
// ============================================================
TEST(DatabasePoolTest, FileMetaLifecycle) {
  MockPool mp(1);

  // store_file_meta
  mp.connections[0]->execute_result = 1;

  FileMeta fm;
  fm.file_hash = "abcdef123";
  fm.file_path = "/music/track.mp3";
  fm.file_size = 2048000;
  fm.content_type = "audio/mpeg";
  EXPECT_TRUE(mp.pool->store_file_meta(fm));

  // get_file_meta
  QueryResult qr;
  qr.columns = {"file_hash", "file_path", "file_size", "content_type", "created_at"};
  qr.rows.push_back({"abcdef123", "/music/track.mp3", "2048000", "audio/mpeg", "2024-07-01 12:00:00"});
  mp.connections[0]->query_result = std::move(qr);

  auto result = mp.pool->get_file_meta("abcdef123");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->file_hash, "abcdef123");
  EXPECT_EQ(result->file_path, "/music/track.mp3");
  EXPECT_EQ(result->file_size, 2048000U);
  EXPECT_EQ(result->content_type, "audio/mpeg");
  EXPECT_EQ(result->created_at, "2024-07-01 12:00:00");
}

TEST(DatabasePoolTest, FileMetaNotFound) {
  MockPool mp(1);

  mp.connections[0]->query_result = std::nullopt;

  auto result = mp.pool->get_file_meta("nonexistent");
  EXPECT_FALSE(result.has_value());
}

} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
