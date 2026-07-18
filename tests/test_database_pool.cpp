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
// T3: User 模型 role 字段
// ============================================================
TEST(DatabaseModelTest, UserModelRole) {
  User u;
  EXPECT_EQ(u.role, UserRole::GUEST);

  u.role = UserRole::NORMAL;
  EXPECT_EQ(u.role, UserRole::NORMAL);

  u.role = UserRole::VIP;
  EXPECT_EQ(u.role, UserRole::VIP);
}

// ============================================================
// T4: FileRecord 模型字段
// ============================================================
TEST(DatabaseModelTest, FileRecordModelFields) {
  FileRecord r;
  EXPECT_EQ(r.file_id, 0);
  EXPECT_TRUE(r.file_name.empty());
  EXPECT_TRUE(r.file_hash.empty());
  EXPECT_EQ(r.file_size, 0U);
  EXPECT_TRUE(r.content_type.empty());
  EXPECT_EQ(r.chunk_size, 2097152);
  EXPECT_TRUE(r.created_at.empty());

  r.file_id = 42;
  r.file_name = "song.mp3";
  r.file_hash = "abcdef";
  r.file_size = 2048000;
  r.content_type = "audio/mpeg";
  r.chunk_size = 1048576;

  EXPECT_EQ(r.file_id, 42);
  EXPECT_EQ(r.file_name, "song.mp3");
  EXPECT_EQ(r.file_hash, "abcdef");
  EXPECT_EQ(r.file_size, 2048000U);
  EXPECT_EQ(r.content_type, "audio/mpeg");
  EXPECT_EQ(r.chunk_size, 1048576);
}

// ============================================================
// T5: FileChunkRecord 模型字段
// ============================================================
TEST(DatabaseModelTest, FileChunkRecordModelFields) {
  FileChunkRecord c;
  EXPECT_TRUE(c.file_hash.empty());
  EXPECT_EQ(c.chunk_index, 0);
  EXPECT_TRUE(c.chunk_hash.empty());
  EXPECT_EQ(c.chunk_offset, 0U);
  EXPECT_EQ(c.chunk_size, 0);

  c.file_hash = "abcdef";
  c.chunk_index = 3;
  c.chunk_hash = "chunkhash123";
  c.chunk_offset = 6291456;
  c.chunk_size = 2097152;

  EXPECT_EQ(c.file_hash, "abcdef");
  EXPECT_EQ(c.chunk_index, 3);
  EXPECT_EQ(c.chunk_hash, "chunkhash123");
  EXPECT_EQ(c.chunk_offset, 6291456U);
  EXPECT_EQ(c.chunk_size, 2097152);
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
  EXPECT_LE(elapsed.count(), 1000);

  release_hold.set_value();
  auto result1 = fut1.get();
  EXPECT_FALSE(result1.has_value());
}

// ============================================================
// T8: get_user
// ============================================================
TEST(DatabasePoolTest, GetUser) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {"user_id", "username", "password_hash", "role", "email", "created_at"};
  qr.rows.push_back({"42", "bob", "hash123", "1", "bob@test.com", "2024-01-15"});
  mp.connections[0]->query_result = std::move(qr);

  auto user = mp.pool->get_user(42);
  ASSERT_TRUE(user.has_value());
  EXPECT_EQ(user->user_id, 42);
  EXPECT_EQ(user->username, "bob");
  EXPECT_EQ(user->password_hash, "hash123");
  EXPECT_EQ(user->email, "bob@test.com");
  EXPECT_EQ(user->created_at, "2024-01-15");
  EXPECT_EQ(user->role, UserRole::NORMAL);
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
  ASSERT_EQ(mp.connections[0]->last_params.size(), 4U);
  EXPECT_EQ(mp.connections[0]->last_params[0], "carol");
  EXPECT_EQ(mp.connections[0]->last_params[1], "secure_hash");
  EXPECT_EQ(mp.connections[0]->last_params[2], "0");
  EXPECT_EQ(mp.connections[0]->last_params[3], "carol@test.com");
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
// T10: StoreAndGetFileRecord
// ============================================================
TEST(DatabasePoolTest, StoreAndGetFileRecord) {
  MockPool mp(1);

  mp.connections[0]->execute_result = 1;

  FileRecord r;
  r.file_name = "record_test.bin";
  r.file_hash = "testhash123";
  r.file_size = 512000;
  r.content_type = "application/octet-stream";
  EXPECT_TRUE(mp.pool->store_file_record(r));

  QueryResult qr;
  qr.columns = {"file_id", "file_name", "file_hash", "file_size", "content_type", "chunk_size", "created_at"};
  qr.rows.push_back(
    {"1", "record_test.bin", "testhash123", "512000", "application/octet-stream", "2097152", "2024-07-01"});
  mp.connections[0]->query_result = std::move(qr);

  auto result = mp.pool->get_file_record(1);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->file_name, "record_test.bin");
  EXPECT_EQ(result->file_hash, "testhash123");
  EXPECT_EQ(result->file_size, 512000U);
}

TEST(DatabasePoolTest, GetFileRecordByHash) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {"file_id", "file_name", "file_hash", "file_size", "content_type", "chunk_size", "created_at"};
  qr.rows.push_back(
    {"2", "hash_lookup.bin", "byhash789", "256000", "application/octet-stream", "2097152", "2024-07-02"});
  mp.connections[0]->query_result = std::move(qr);

  auto result = mp.pool->get_file_record_by_hash("byhash789");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->file_id, 2);
  EXPECT_EQ(result->file_name, "hash_lookup.bin");
  EXPECT_EQ(result->file_hash, "byhash789");
}

// ============================================================
// T11: SearchFiles
// ============================================================
TEST(DatabasePoolTest, SearchFiles) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {"file_id", "file_name", "file_hash", "file_size", "content_type", "chunk_size", "created_at"};
  qr.rows.push_back({"3", "song1.mp3", "hash1", "1000", "audio/mpeg", "2097152", "2024-07-01"});
  qr.rows.push_back({"4", "song2.mp3", "hash2", "2000", "audio/mpeg", "2097152", "2024-07-02"});
  mp.connections[0]->query_result = std::move(qr);

  auto results = mp.pool->search_files("song", 0, 10);
  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0].file_name, "song1.mp3");
  EXPECT_EQ(results[1].file_name, "song2.mp3");
}

// ============================================================
// T12: StoreAndGetFileChunks + ChunkExists
// ============================================================
TEST(DatabasePoolTest, StoreAndGetFileChunks) {
  MockPool mp(1);

  mp.connections[0]->execute_result = 1;

  FileChunkRecord c1, c2;
  c1.file_hash = "filehash1";
  c1.chunk_index = 0;
  c1.chunk_hash = "chunkhash_a";
  c1.chunk_offset = 0;
  c1.chunk_size = 2097152;

  c2.file_hash = "filehash1";
  c2.chunk_index = 1;
  c2.chunk_hash = "chunkhash_b";
  c2.chunk_offset = 2097152;
  c2.chunk_size = 1048576;

  EXPECT_TRUE(mp.pool->store_file_chunks({c1, c2}));

  QueryResult qr;
  qr.columns = {"file_hash", "chunk_index", "chunk_hash", "chunk_offset", "chunk_size"};
  qr.rows.push_back({"filehash1", "0", "chunkhash_a", "0", "2097152"});
  qr.rows.push_back({"filehash1", "1", "chunkhash_b", "2097152", "1048576"});
  mp.connections[0]->query_result = std::move(qr);

  auto chunks = mp.pool->get_file_chunks("filehash1");
  ASSERT_EQ(chunks.size(), 2U);
  EXPECT_EQ(chunks[0].chunk_index, 0);
  EXPECT_EQ(chunks[0].chunk_hash, "chunkhash_a");
  EXPECT_EQ(chunks[1].chunk_index, 1);
  EXPECT_EQ(chunks[1].chunk_hash, "chunkhash_b");
}

TEST(DatabasePoolTest, ChunkExists) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {"1"};
  qr.rows.push_back({"1"});
  mp.connections[0]->query_result = std::move(qr);

  EXPECT_TRUE(mp.pool->chunk_exists("chunkhash_a"));

  mp.connections[0]->query_result = std::nullopt;
  EXPECT_FALSE(mp.pool->chunk_exists("nonexistent"));
}

// ============================================================
// T13: AuthUser + VerifyPassword
// ============================================================
TEST(DatabasePoolTest, AuthUserQuery) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {"user_id", "username", "role"};
  qr.rows.push_back({"10", "testuser", "1"});
  mp.connections[0]->query_result = std::move(qr);

  auto user = mp.pool->get_auth_user("testuser");
  ASSERT_TRUE(user.has_value());
  EXPECT_EQ(user->user_id, 10);
  EXPECT_EQ(user->username, "testuser");
  EXPECT_EQ(user->role, UserRole::NORMAL);
}

TEST(DatabasePoolTest, VerifyPassword) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {"password_hash"};
  qr.rows.push_back({"correct_hash"});
  mp.connections[0]->query_result = std::move(qr);

  EXPECT_TRUE(mp.pool->verify_password("testuser", "correct_hash"));
  EXPECT_FALSE(mp.pool->verify_password("testuser", "wrong_hash"));
}

} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
