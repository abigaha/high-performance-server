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
  EXPECT_TRUE(u.salt.empty());
  EXPECT_TRUE(u.email.empty());
  EXPECT_TRUE(u.created_at.empty());

  u.user_id = 42;
  u.username = "alice";
  u.password_hash = "abc123";
  u.salt = "salt";
  u.email = "alice@test.com";
  u.created_at = "2024-01-01";

  EXPECT_EQ(u.user_id, 42);
  EXPECT_EQ(u.username, "alice");
  EXPECT_EQ(u.password_hash, "abc123");
  EXPECT_EQ(u.salt, "salt");
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
// T4: FileRecord 模型字段（含扩展字段 music_id, uploaded_by）
// ============================================================
TEST(DatabaseModelTest, FileRecordModelFields) {
  FileRecord r;
  EXPECT_EQ(r.file_id, 0);
  EXPECT_EQ(r.music_id, 0);
  EXPECT_TRUE(r.file_name.empty());
  EXPECT_TRUE(r.file_hash.empty());
  EXPECT_EQ(r.file_size, 0U);
  EXPECT_TRUE(r.content_type.empty());
  EXPECT_EQ(r.chunk_size, 2097152);
  EXPECT_EQ(r.uploaded_by, 0);
  EXPECT_TRUE(r.created_at.empty());

  r.file_id = 42;
  r.music_id = 7;
  r.file_name = "song.mp3";
  r.file_hash = "abcdef";
  r.file_size = 2048000;
  r.content_type = "audio/mpeg";
  r.chunk_size = 1048576;
  r.uploaded_by = 99;

  EXPECT_EQ(r.file_id, 42);
  EXPECT_EQ(r.music_id, 7);
  EXPECT_EQ(r.file_name, "song.mp3");
  EXPECT_EQ(r.file_hash, "abcdef");
  EXPECT_EQ(r.file_size, 2048000U);
  EXPECT_EQ(r.content_type, "audio/mpeg");
  EXPECT_EQ(r.chunk_size, 1048576);
  EXPECT_EQ(r.uploaded_by, 99);
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
  qr.columns = {"user_id", "username", "password_hash", "salt", "role", "email", "created_at"};
  qr.rows.push_back({"42", "bob", "hash123", "somesalt", "1", "bob@test.com", "2024-01-15"});
  mp.connections[0]->query_result = std::move(qr);

  auto user = mp.pool->get_user(42);
  ASSERT_TRUE(user.has_value());
  EXPECT_EQ(user->user_id, 42);
  EXPECT_EQ(user->username, "bob");
  EXPECT_EQ(user->password_hash, "hash123");
  EXPECT_EQ(user->salt, "somesalt");
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
  u.salt = "testsalt";
  u.email = "carol@test.com";

  EXPECT_TRUE(mp.pool->create_user(u));

  // 验证 SQL 中含有参数化占位符
  EXPECT_NE(mp.connections[0]->last_sql.find('?'), std::string::npos);
  ASSERT_EQ(mp.connections[0]->last_params.size(), 5U);
  EXPECT_EQ(mp.connections[0]->last_params[0], "carol");
  EXPECT_EQ(mp.connections[0]->last_params[1], "secure_hash");
  EXPECT_EQ(mp.connections[0]->last_params[2], "testsalt");
  EXPECT_EQ(mp.connections[0]->last_params[3], "0");
  EXPECT_EQ(mp.connections[0]->last_params[4], "carol@test.com");
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
  mp.connections[0]->last_insert_id_value = 42;

  FileRecord r;
  r.file_name = "record_test.bin";
  r.file_hash = "testhash123";
  r.file_size = 512000;
  r.content_type = "application/octet-stream";
  auto file_id = mp.pool->store_file_record(r);
  ASSERT_TRUE(file_id.has_value());
  EXPECT_EQ(*file_id, 42);
  EXPECT_EQ(mp.connections[0]->last_insert_id_count, 1);
  EXPECT_NE(mp.connections[0]->last_sql.find("LAST_INSERT_ID(file_id)"), std::string::npos);

  QueryResult qr;
  qr.columns = {"file_id",
                "file_name",
                "file_hash",
                "file_size",
                "content_type",
                "chunk_size",
                "created_at",
                "music_id",
                "uploaded_by"};
  qr.rows.push_back(
    {"1", "record_test.bin", "testhash123", "512000", "application/octet-stream", "2097152", "2024-07-01", "0", "0"});
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
  qr.columns = {"file_id",
                "file_name",
                "file_hash",
                "file_size",
                "content_type",
                "chunk_size",
                "created_at",
                "music_id",
                "uploaded_by"};
  qr.rows.push_back(
    {"2", "hash_lookup.bin", "byhash789", "256000", "application/octet-stream", "2097152", "2024-07-02", "0", "0"});
  mp.connections[0]->query_result = std::move(qr);

  auto result = mp.pool->get_file_record_by_hash("byhash789");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->file_id, 2);
  EXPECT_EQ(result->file_name, "hash_lookup.bin");
  EXPECT_EQ(result->file_hash, "byhash789");
}

TEST(DatabasePoolTest, StoreFileRecordReturnsExistingIdWhenUpsertDoesNotChangeData) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 0;
  mp.connections[0]->last_insert_id_value = 24;

  FileRecord record;
  record.file_name = "existing.mp3";
  record.file_hash = "existing_hash";

  auto file_id = mp.pool->store_file_record(record);

  ASSERT_TRUE(file_id.has_value());
  EXPECT_EQ(*file_id, 24);
}

TEST(DatabasePoolTest, StoreFileRecordFailsWithoutDatabaseResultOrId) {
  MockPool mp(1);
  FileRecord record;
  record.file_name = "failed.bin";
  record.file_hash = "failed_hash";

  mp.connections[0]->execute_result = std::nullopt;
  EXPECT_FALSE(mp.pool->store_file_record(record).has_value());
  EXPECT_EQ(mp.connections[0]->last_insert_id_count, 0);

  mp.connections[0]->execute_result = 1;
  EXPECT_FALSE(mp.pool->store_file_record(record).has_value());
}

// ============================================================
// T11: SearchFiles
// ============================================================
TEST(DatabasePoolTest, SearchFiles) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {"file_id",
                "file_name",
                "file_hash",
                "file_size",
                "content_type",
                "chunk_size",
                "created_at",
                "music_id",
                "uploaded_by"};
  qr.rows.push_back({"3", "song1.mp3", "hash1", "1000", "audio/mpeg", "2097152", "2024-07-01", "0", "0"});
  qr.rows.push_back({"4", "song2.mp3", "hash2", "2000", "audio/mpeg", "2097152", "2024-07-02", "0", "0"});
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

// ============================================================
// T14: MusicMeta 模型字段
// ============================================================
TEST(DatabaseModelTest, MusicMetaModelFields) {
  MusicMeta m;
  EXPECT_EQ(m.music_id, 0);
  EXPECT_TRUE(m.title.empty());
  EXPECT_TRUE(m.artist.empty());
  EXPECT_TRUE(m.album.empty());
  EXPECT_TRUE(m.genre.empty());
  EXPECT_EQ(m.duration_sec, 0);
  EXPECT_EQ(m.track_number, 0);

  m.music_id = 1;
  m.title = "Love Story";
  m.artist = "Taylor Swift";
  m.album = "Fearless";
  m.genre = "Pop";
  m.duration_sec = 240;
  m.track_number = 3;

  EXPECT_EQ(m.music_id, 1);
  EXPECT_EQ(m.title, "Love Story");
  EXPECT_EQ(m.artist, "Taylor Swift");
  EXPECT_EQ(m.album, "Fearless");
  EXPECT_EQ(m.genre, "Pop");
  EXPECT_EQ(m.duration_sec, 240);
  EXPECT_EQ(m.track_number, 3);
}

// ============================================================
// T15: Playlist 模型字段
// ============================================================
TEST(DatabaseModelTest, PlaylistModelFields) {
  Playlist p;
  EXPECT_EQ(p.playlist_id, 0);
  EXPECT_EQ(p.user_id, 0);
  EXPECT_TRUE(p.name.empty());
  EXPECT_TRUE(p.description.empty());
  EXPECT_EQ(p.item_count, 0);

  p.playlist_id = 5;
  p.user_id = 1;
  p.name = "我的最爱";
  p.description = "我最喜欢的歌";
  p.item_count = 10;

  EXPECT_EQ(p.playlist_id, 5);
  EXPECT_EQ(p.user_id, 1);
  EXPECT_EQ(p.name, "我的最爱");
  EXPECT_EQ(p.description, "我最喜欢的歌");
  EXPECT_EQ(p.item_count, 10);
}

// ============================================================
// T16: PlaylistItem 模型字段
// ============================================================
TEST(DatabaseModelTest, PlaylistItemModelFields) {
  PlaylistItem pi;
  EXPECT_EQ(pi.id, 0);
  EXPECT_EQ(pi.playlist_id, 0);
  EXPECT_EQ(pi.music_id, 0);
  EXPECT_TRUE(pi.title.empty());
  EXPECT_TRUE(pi.file_hash.empty());
  EXPECT_EQ(pi.sort_order, 0);

  pi.id = 100;
  pi.playlist_id = 5;
  pi.music_id = 3;
  pi.title = "Song";
  pi.artist = "Artist";
  pi.file_hash = "abc123";
  pi.sort_order = 1;

  EXPECT_EQ(pi.id, 100);
  EXPECT_EQ(pi.playlist_id, 5);
  EXPECT_EQ(pi.music_id, 3);
  EXPECT_EQ(pi.title, "Song");
  EXPECT_EQ(pi.artist, "Artist");
  EXPECT_EQ(pi.file_hash, "abc123");
  EXPECT_EQ(pi.sort_order, 1);
}

// ============================================================
// T17: username_exists
// ============================================================
TEST(DatabasePoolTest, UsernameExists) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {"1"};
  qr.rows.push_back({"1"});
  mp.connections[0]->query_result = std::move(qr);

  EXPECT_TRUE(mp.pool->username_exists("existing_user"));

  mp.connections[0]->query_result = std::nullopt;
  EXPECT_FALSE(mp.pool->username_exists("nonexistent"));
}

// ============================================================
// T18: search_files_ext — type 过滤 + total
// ============================================================
TEST(DatabasePoolTest, SearchFilesExt) {
  MockPool mp(1);

  // count query
  QueryResult count_qr;
  count_qr.columns = {"COUNT(*)"};
  count_qr.rows.push_back({"1"});
  mp.connections[0]->query_result = std::move(count_qr);

  // data query
  QueryResult data_qr;
  data_qr.columns = {"file_id",
                     "file_name",
                     "file_hash",
                     "file_size",
                     "content_type",
                     "chunk_size",
                     "created_at",
                     "music_id",
                     "uploaded_by"};
  data_qr.rows.push_back({"1", "song.mp3", "hash1", "1000", "audio/mpeg", "2097152", "2024-01-01", "1", "1"});
  mp.connections[0]->query_result = std::move(data_qr);

  int total = 0;
  auto results = mp.pool->search_files_ext("song", "audio", 0, 10, total);
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(total, 1);
  EXPECT_EQ(results[0].file_name, "song.mp3");
  EXPECT_EQ(results[0].music_id, 1);
}

// ============================================================
// T19: delete_file_record
// ============================================================
TEST(DatabasePoolTest, DeleteFileRecord) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;
  EXPECT_TRUE(mp.pool->delete_file_record(1));
}

// ============================================================
// T20: update_file_record
// ============================================================
TEST(DatabasePoolTest, UpdateFileRecord) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;

  FileRecord r;
  r.file_id = 1;
  r.file_name = "updated.mp3";
  r.content_type = "audio/mpeg";
  r.music_id = 5;
  r.uploaded_by = 1;

  EXPECT_TRUE(mp.pool->update_file_record(r));
  EXPECT_NE(mp.connections[0]->last_sql.find("UPDATE file_records"), std::string::npos);
}

// ============================================================
// T21: update_user
// ============================================================
TEST(DatabasePoolTest, UpdateUser) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;

  User u;
  u.user_id = 1;
  u.email = "new@test.com";
  u.password_hash = "new_hash";
  u.salt = "new_salt";

  EXPECT_TRUE(mp.pool->update_user(u));
  EXPECT_NE(mp.connections[0]->last_sql.find("salt = ?"), std::string::npos);
  ASSERT_EQ(mp.connections[0]->last_params.size(), 4U);
  EXPECT_EQ(mp.connections[0]->last_params[0], "new@test.com");
  EXPECT_EQ(mp.connections[0]->last_params[1], "new_hash");
  EXPECT_EQ(mp.connections[0]->last_params[2], "new_salt");
  EXPECT_EQ(mp.connections[0]->last_params[3], "1");
}

TEST(DatabasePoolTest, ListMusicLibraryUsesExistsWithoutInvalidGroupBy) {
  MockPool mp(1);
  std::vector<std::string> sqls;
  mp.connections[0]->query_hook = [&sqls](const std::string& sql,
                                          const std::vector<std::string>&) -> std::optional<QueryResult> {
    sqls.push_back(sql);
    QueryResult result;
    if (sqls.size() == 1U) {
      result.columns = {"COUNT(*)"};
      result.rows.push_back({"1"});
      return result;
    }
    result.columns = {
      "music_id", "title", "artist", "album", "genre", "duration_sec", "track_number", "created_at", "updated_at"};
    result.rows.push_back({"7", "Song", "Artist", "Album", "Pop", "180", "2", "2024-01-01", "2024-01-02"});
    return result;
  };

  int total = 0;
  auto items = mp.pool->list_music_library("song", 0, 10, total);

  EXPECT_EQ(total, 1);
  ASSERT_EQ(items.size(), 1U);
  EXPECT_EQ(items[0].music_id, 7);
  ASSERT_EQ(sqls.size(), 2U);
  EXPECT_NE(sqls[0].find("EXISTS"), std::string::npos);
  EXPECT_EQ(sqls[1].find("GROUP BY"), std::string::npos);
  EXPECT_EQ(sqls[1].find("file_hash"), std::string::npos);
}

// ============================================================
// T22: create_music_meta + last_insert_id
// ============================================================
TEST(DatabasePoolTest, CreateMusicMeta) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;
  mp.connections[0]->last_insert_id_value = 42;

  MusicMeta m;
  m.title = "Test Song";
  m.artist = "Artist";
  m.album = "Album";
  m.genre = "Pop";
  m.duration_sec = 200;

  auto id = mp.pool->create_music_meta(m);
  EXPECT_EQ(id, 42);
}

// ============================================================
// T23: get_music_meta — 存在 / 不存在
// ============================================================
TEST(DatabasePoolTest, GetMusicMeta) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {
    "music_id", "title", "artist", "album", "genre", "duration_sec", "track_number", "created_at", "updated_at"};
  qr.rows.push_back({"1", "Song", "Artist", "Album", "Pop", "200", "3", "2024-01-01", "2024-01-01"});
  mp.connections[0]->query_result = std::move(qr);

  auto meta = mp.pool->get_music_meta(1);
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->title, "Song");
  EXPECT_EQ(meta->artist, "Artist");

  mp.connections[0]->query_result = std::nullopt;
  EXPECT_FALSE(mp.pool->get_music_meta(999).has_value());
}

// ============================================================
// T24: update_music_meta
// ============================================================
TEST(DatabasePoolTest, UpdateMusicMeta) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;

  MusicMeta m;
  m.music_id = 1;
  m.title = "Updated";

  EXPECT_TRUE(mp.pool->update_music_meta(m));
}

// ============================================================
// T25: delete_music_meta
// ============================================================
TEST(DatabasePoolTest, DeleteMusicMeta) {
  MockPool mp(1);

  // First call: UPDATE file_records SET music_id = NULL
  mp.connections[0]->execute_result = 1;

  EXPECT_TRUE(mp.pool->delete_music_meta(1));
}

// ============================================================
// T26: get_user_playlists
// ============================================================
TEST(DatabasePoolTest, GetUserPlaylists) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {"playlist_id", "user_id", "name", "description", "item_count", "created_at"};
  qr.rows.push_back({"1", "1", "Favorites", "My favs", "3", "2024-01-01"});
  mp.connections[0]->query_result = std::move(qr);

  auto playlists = mp.pool->get_user_playlists(1);
  ASSERT_EQ(playlists.size(), 1U);
  EXPECT_EQ(playlists[0].name, "Favorites");
  EXPECT_EQ(playlists[0].item_count, 3);
}

// ============================================================
// T27: create_playlist + last_insert_id
// ============================================================
TEST(DatabasePoolTest, CreatePlaylist) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;
  mp.connections[0]->last_insert_id_value = 10;

  Playlist pl;
  pl.user_id = 1;
  pl.name = "My Playlist";

  auto id = mp.pool->create_playlist(pl);
  EXPECT_EQ(id, 10);
}

// ============================================================
// T28: add_playlist_item + remove_playlist_item + reorder
// ============================================================
TEST(DatabasePoolTest, AddRemoveReorderPlaylistItems) {
  MockPool mp(1);

  // add: max_order query returns null → next_order = 0
  QueryResult max_qr;
  max_qr.columns = {"next"};
  max_qr.rows.push_back({"0"});
  mp.connections[0]->query_result = std::move(max_qr);
  mp.connections[0]->execute_result = 1;

  EXPECT_TRUE(mp.pool->add_playlist_item(1, 5));

  // remove
  mp.connections[0]->execute_result = 1;
  EXPECT_TRUE(mp.pool->remove_playlist_item(1, 5));

  // reorder: DELETE + 3 INSERT
  mp.connections[0]->execute_result = 1;
  EXPECT_TRUE(mp.pool->reorder_playlist_items(1, {3, 1, 2}));
}

// ============================================================
// T29: get_playlist_items (STRAIGHT_JOIN)
// ============================================================
TEST(DatabasePoolTest, GetPlaylistItems) {
  MockPool mp(1);

  QueryResult qr;
  qr.columns = {"id", "playlist_id", "music_id", "sort_order", "added_at", "title", "artist", "file_hash"};
  qr.rows.push_back({"1", "1", "5", "0", "2024-01-01", "Song", "Artist", "abcdef"});
  mp.connections[0]->query_result = std::move(qr);

  auto items = mp.pool->get_playlist_items(1);
  ASSERT_EQ(items.size(), 1U);
  EXPECT_EQ(items[0].title, "Song");
  EXPECT_EQ(items[0].artist, "Artist");
  EXPECT_EQ(items[0].file_hash, "abcdef");
  EXPECT_EQ(items[0].sort_order, 0);
}

} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
