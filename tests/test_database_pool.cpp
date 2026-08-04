#include "database_pool.h"
#include "db_config.h"
#include "iconnection.h"
#include "mock_connection.h"
#include "models.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace hps {

// 工厂辅助：创建使用 MockConnection 的 DatabasePool
struct MockPool {
  ChunkLifecycleCoordinator coordinator;
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
    if (!pool->bind_chunk_lifecycle_coordinator(coordinator)) {
      throw std::runtime_error("failed to bind chunk lifecycle coordinator");
    }
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

TEST(DatabasePoolFileDeletionTest, OwnerQueuesOnlyChunksWithNoRemainingReferencesAndKeepsSharedMusic) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  std::vector<std::string> executed;
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    executed.push_back(sql);
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>& params) {
    if (sql == "SELECT music_id FROM file_records WHERE file_id = ?") {
      return std::optional<QueryResult>{QueryResult{.rows = {{"9"}}}};
    }
    if (sql.find("FROM music_meta") != std::string::npos) {
      return std::optional<QueryResult>{QueryResult{.rows = {{"9"}}}};
    }
    if (sql.find("WHERE file_id = ?") != std::string::npos) {
      return std::optional<QueryResult>{
        QueryResult{.rows = {{"7", "owner.mp3", "file-hash", "10", "audio/mpeg", "4", "2026-01-01", "9", "42"}}}};
    }
    if (sql.find("WHERE chunk_hash = ? ORDER BY file_hash, chunk_index FOR UPDATE") != std::string::npos) {
      return std::optional<QueryResult>{
        QueryResult{.rows = params[0] == "shared" ? std::vector<std::vector<std::string>>{{"file-hash"}, {"other-file"}}
                                                  : std::vector<std::vector<std::string>>{{"file-hash"}}}};
    }
    if (sql.find("FROM file_chunks") != std::string::npos) {
      return std::optional<QueryResult>{QueryResult{.rows = {{"only"}, {"shared"}}}};
    }
    if (sql.find("COUNT(*) FROM file_records WHERE music_id") != std::string::npos) {
      return std::optional<QueryResult>{QueryResult{.rows = {{"1"}}}};
    }
    return std::optional<QueryResult>{QueryResult{}};
  };

  auto cleanup_guard = mp.coordinator.acquire_cleanup_guard();
  const auto result = mp.pool->delete_file_owned(cleanup_guard.permit(), 7, 42, false);

  ASSERT_EQ(result.status, MutationStatus::OK) << result.detail.value_or("no detail");
  ASSERT_TRUE(result.value);
  EXPECT_EQ(result.value->file_id, 7);
  EXPECT_EQ(result.value->queued_chunk_count, 1U);
  EXPECT_TRUE(std::ranges::any_of(executed, [](const auto& sql) {
    return sql.find("pending_chunk_deletions") != std::string::npos;
  }));
  EXPECT_FALSE(std::ranges::any_of(executed, [](const auto& sql) {
    return sql.find("DELETE FROM music_meta") != std::string::npos;
  }));
}

TEST(DatabasePoolFileDeletionTest, CanonicalCoordinatorCanOnlyBindToOneIdentity) {
  MockPool mp(1);
  ChunkLifecycleCoordinator other;

  EXPECT_TRUE(mp.pool->bind_chunk_lifecycle_coordinator(mp.coordinator));
  EXPECT_TRUE(mp.pool->bind_chunk_lifecycle_coordinator(mp.coordinator));
  EXPECT_FALSE(mp.pool->bind_chunk_lifecycle_coordinator(other));
}

TEST(DatabasePoolFileDeletionTest, WrongCleanupPermitIsRejectedBeforeStartingTransaction) {
  MockPool mp(1);
  ChunkLifecycleCoordinator other;
  auto wrong_guard = other.acquire_cleanup_guard();
  std::vector<std::string> executed;
  mp.connections[0]->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    executed.push_back(sql);
    return std::optional<int64_t>{1};
  };

  const auto result = mp.pool->delete_file_owned(wrong_guard.permit(), 7, 42, false);

  EXPECT_EQ(result.status, MutationStatus::INVALID_STATE);
  EXPECT_EQ(result.detail, "CLEANUP_PERMIT_INVALID");
  EXPECT_TRUE(executed.empty());
}

TEST(DatabasePoolFileDeletionTest, NonOwnerIsRejectedWithoutDeletingOrEnqueueing) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  std::vector<std::string> executed;
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    executed.push_back(sql);
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) {
    if (sql == "SELECT music_id FROM file_records WHERE file_id = ?")
      return std::optional<QueryResult>{QueryResult{.rows = {{""}}}};
    return std::optional<QueryResult>{
      QueryResult{.rows = {{"7", "owner.mp3", "file-hash", "10", "audio/mpeg", "4", "2026-01-01", "", "42"}}}};
  };

  auto cleanup_guard = mp.coordinator.acquire_cleanup_guard();
  const auto result = mp.pool->delete_file_owned(cleanup_guard.permit(), 7, 99, false);

  EXPECT_EQ(result.status, MutationStatus::OWNER_REQUIRED);
  EXPECT_FALSE(result.value);
  EXPECT_FALSE(std::ranges::any_of(executed, [](const auto& sql) {
    return sql.find("DELETE FROM file_records") != std::string::npos ||
           sql.find("pending_chunk_deletions") != std::string::npos;
  }));
  EXPECT_TRUE(std::ranges::any_of(executed, [](const auto& sql) { return sql == "ROLLBACK"; }));
}

TEST(DatabasePoolFileDeletionTest, ConcurrentDeletesLockChunkHashesInSameGlobalOrder) {
  MockPool mp(2);
  std::barrier chunks_loaded(2);
  std::mutex capture_mutex;
  std::vector<std::vector<std::string>> lock_orders(2);
  for (std::size_t index = 0; index < mp.connections.size(); ++index) {
    auto* connection = mp.connections[index];
    connection->execute_hook = [](const std::string&, const std::vector<std::string>&) {
      return std::optional<int64_t>{1};
    };
    connection->query_hook = [&, index](const std::string& sql, const std::vector<std::string>& params) {
      if (sql == "SELECT music_id FROM file_records WHERE file_id = ?") {
        return std::optional<QueryResult>{QueryResult{.rows = {{""}}}};
      }
      if (sql.find("WHERE file_id = ?") != std::string::npos) {
        return std::optional<QueryResult>{QueryResult{
          .rows = {
            {params[0], "file", "file-" + params[0], "10", "application/octet-stream", "4", "2026-01-01", "0", "42"}}}};
      }
      if (sql.find("WHERE file_hash = ? ORDER BY chunk_index") != std::string::npos) {
        chunks_loaded.arrive_and_wait();
        const bool reverse = params[0] == "file-8";
        return std::optional<QueryResult>{QueryResult{.rows = reverse
                                                                ? std::vector<std::vector<std::string>>{{"z"}, {"a"}}
                                                                : std::vector<std::vector<std::string>>{{"a"}, {"z"}}}};
      }
      if (sql.find("WHERE chunk_hash = ? ORDER BY file_hash, chunk_index FOR UPDATE") != std::string::npos) {
        std::lock_guard lock(capture_mutex);
        lock_orders[index].push_back(params[0]);
        return std::optional<QueryResult>{QueryResult{.rows = {{"file-" + std::to_string(index == 0 ? 7 : 8)}}}};
      }
      return std::optional<QueryResult>{QueryResult{}};
    };
  }

  auto cleanup_guard = mp.coordinator.acquire_cleanup_guard();
  auto first =
    std::async(std::launch::async, [&] { return mp.pool->delete_file_owned(cleanup_guard.permit(), 7, 42, false); });
  auto second =
    std::async(std::launch::async, [&] { return mp.pool->delete_file_owned(cleanup_guard.permit(), 8, 42, false); });

  EXPECT_EQ(first.get().status, MutationStatus::OK);
  EXPECT_EQ(second.get().status, MutationStatus::OK);
  EXPECT_EQ(lock_orders[0], (std::vector<std::string>{"a", "z"}));
  EXPECT_EQ(lock_orders[1], (std::vector<std::string>{"a", "z"}));
}

TEST(DatabasePoolFileDeletionTest, DuplicateChunkHashIsLockedAndQueuedOnlyOnce) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  int pending_inserts = 0;
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    if (sql.find("pending_chunk_deletions") != std::string::npos)
      ++pending_inserts;
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>& params) {
    if (sql == "SELECT music_id FROM file_records WHERE file_id = ?") {
      return std::optional<QueryResult>{QueryResult{.rows = {{""}}}};
    }
    if (sql.find("WHERE file_id = ?") != std::string::npos) {
      return std::optional<QueryResult>{
        QueryResult{.rows = {{"7", "file", "file-7", "10", "application/octet-stream", "4", "2026-01-01", "0", "42"}}}};
    }
    if (sql.find("WHERE file_hash = ? ORDER BY chunk_index") != std::string::npos) {
      return std::optional<QueryResult>{QueryResult{.rows = {{"same"}, {"same"}}}};
    }
    if (sql.find("WHERE chunk_hash = ? ORDER BY file_hash, chunk_index FOR UPDATE") != std::string::npos) {
      EXPECT_EQ(params[0], "same");
      return std::optional<QueryResult>{QueryResult{.rows = {{"file-7"}, {"file-7"}}}};
    }
    return std::optional<QueryResult>{QueryResult{}};
  };

  auto cleanup_guard = mp.coordinator.acquire_cleanup_guard();
  const auto result = mp.pool->delete_file_owned(cleanup_guard.permit(), 7, 42, false);

  ASSERT_EQ(result.status, MutationStatus::OK);
  EXPECT_EQ(result.value->queued_chunk_count, 1U);
  EXPECT_EQ(pending_inserts, 1);
}

TEST(DatabasePoolFileDeletionTest, MusicIsLockedBeforeTargetFileAndThenRevalidated) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  std::vector<std::string> queries;
  connection->execute_hook = [](const std::string&, const std::vector<std::string>&) {
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    queries.push_back(sql);
    if (sql.find("SELECT music_id FROM file_records") != std::string::npos)
      return std::optional<QueryResult>{QueryResult{.rows = {{"9"}}}};
    if (sql.find("FROM music_meta") != std::string::npos)
      return std::optional<QueryResult>{QueryResult{.rows = {{"9"}}}};
    if (sql.find("WHERE file_id = ? FOR UPDATE") != std::string::npos)
      return std::optional<QueryResult>{
        QueryResult{.rows = {{"7", "owner.mp3", "file-hash", "10", "audio/mpeg", "4", "2026-01-01", "9", "42"}}}};
    if (sql.find("COUNT(*) FROM file_records WHERE music_id") != std::string::npos)
      return std::optional<QueryResult>{QueryResult{.rows = {{"0"}}}};
    return std::optional<QueryResult>{QueryResult{}};
  };

  auto cleanup_guard = mp.coordinator.acquire_cleanup_guard();
  const auto result = mp.pool->delete_file_owned(cleanup_guard.permit(), 7, 42, false);

  ASSERT_EQ(result.status, MutationStatus::OK) << result.detail.value_or("no detail");
  ASSERT_GE(queries.size(), 3U);
  EXPECT_EQ(queries[0], "SELECT music_id FROM file_records WHERE file_id = ?");
  EXPECT_EQ(queries[1], "SELECT music_id FROM music_meta WHERE music_id = ? FOR UPDATE");
  EXPECT_NE(queries[2].find("WHERE file_id = ? FOR UPDATE"), std::string::npos);
}

TEST(DatabasePoolFileDeletionTest, ConcurrentSameMusicTransactionsReachSharedMusicLockBeforeEitherFileLock) {
  MockPool mp(2);
  std::barrier music_lock_requested(2);
  std::atomic<int> music_lock_requests{0};
  std::atomic<int> file_locks_before_music_barrier{0};
  for (auto* connection : mp.connections) {
    connection->execute_hook = [](const std::string&, const std::vector<std::string>&) {
      return std::optional<int64_t>{1};
    };
    connection->query_hook = [&](const std::string& sql, const std::vector<std::string>& params) {
      if (sql.find("SELECT music_id FROM file_records") != std::string::npos)
        return std::optional<QueryResult>{QueryResult{.rows = {{"9"}}}};
      if (sql.find("FROM music_meta") != std::string::npos) {
        music_lock_requests.fetch_add(1);
        music_lock_requested.arrive_and_wait();
        return std::optional<QueryResult>{QueryResult{.rows = {{"9"}}}};
      }
      if (sql.find("WHERE file_id = ? FOR UPDATE") != std::string::npos) {
        if (music_lock_requests.load() < 2)
          file_locks_before_music_barrier.fetch_add(1);
        return std::optional<QueryResult>{QueryResult{
          .rows = {{params[0], "owner.mp3", "file-" + params[0], "10", "audio/mpeg", "4", "2026-01-01", "9", "42"}}}};
      }
      if (sql.find("COUNT(*) FROM file_records WHERE music_id") != std::string::npos)
        return std::optional<QueryResult>{QueryResult{.rows = {{"1"}}}};
      return std::optional<QueryResult>{QueryResult{}};
    };
  }

  auto cleanup_guard = mp.coordinator.acquire_cleanup_guard();
  auto first =
    std::async(std::launch::async, [&] { return mp.pool->delete_file_owned(cleanup_guard.permit(), 7, 42, false); });
  auto second =
    std::async(std::launch::async, [&] { return mp.pool->delete_file_owned(cleanup_guard.permit(), 8, 42, false); });

  EXPECT_EQ(first.get().status, MutationStatus::OK);
  EXPECT_EQ(second.get().status, MutationStatus::OK);
  EXPECT_EQ(file_locks_before_music_barrier.load(), 0);
}

TEST(DatabasePoolFileDeletionTest, MalformedChunkListReturnsInvalidStateAndRollsBack) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  std::vector<std::string> executed;
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    executed.push_back(sql);
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) {
    if (sql.find("SELECT music_id FROM file_records") != std::string::npos)
      return std::optional<QueryResult>{QueryResult{.rows = {{""}}}};
    if (sql.find("WHERE file_id = ? FOR UPDATE") != std::string::npos)
      return std::optional<QueryResult>{QueryResult{
        .rows = {{"7", "owner.bin", "file-hash", "10", "application/octet-stream", "4", "2026-01-01", "", "42"}}}};
    if (sql.find("FROM file_chunks") != std::string::npos)
      return std::optional<QueryResult>{QueryResult{.rows = {{"hash", "unexpected"}}}};
    return std::optional<QueryResult>{QueryResult{}};
  };

  auto cleanup_guard = mp.coordinator.acquire_cleanup_guard();
  const auto result = mp.pool->delete_file_owned(cleanup_guard.permit(), 7, 42, false);

  EXPECT_EQ(result.status, MutationStatus::INVALID_STATE);
  EXPECT_EQ(result.detail, "FILE_CHUNK_STATE_INVALID");
  EXPECT_TRUE(std::ranges::any_of(executed, [](const auto& sql) { return sql == "ROLLBACK"; }));
  EXPECT_FALSE(std::ranges::any_of(executed, [](const auto& sql) { return sql == "COMMIT"; }));
}

TEST(DatabasePoolFileDeletionTest, MusicChangeAfterPreviewReturnsConflictAndRollsBack) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  std::vector<std::string> executed;
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    executed.push_back(sql);
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) {
    if (sql == "SELECT music_id FROM file_records WHERE file_id = ?")
      return std::optional<QueryResult>{QueryResult{.rows = {{"9"}}}};
    if (sql.find("FROM music_meta") != std::string::npos)
      return std::optional<QueryResult>{QueryResult{.rows = {{"9"}}}};
    if (sql.find("WHERE file_id = ? FOR UPDATE") != std::string::npos)
      return std::optional<QueryResult>{
        QueryResult{.rows = {{"7", "owner.mp3", "file-hash", "10", "audio/mpeg", "4", "2026-01-01", "10", "42"}}}};
    return std::optional<QueryResult>{QueryResult{}};
  };

  auto cleanup_guard = mp.coordinator.acquire_cleanup_guard();
  const auto result = mp.pool->delete_file_owned(cleanup_guard.permit(), 7, 42, false);

  EXPECT_EQ(result.status, MutationStatus::CONFLICT);
  EXPECT_EQ(result.detail, "FILE_MUSIC_CHANGED");
  EXPECT_TRUE(std::ranges::any_of(executed, [](const auto& sql) { return sql == "ROLLBACK"; }));
  EXPECT_FALSE(std::ranges::any_of(executed, [](const auto& sql) {
    return sql.find("DELETE FROM file_records") != std::string::npos;
  }));
}

TEST(DatabasePoolPendingDeletionTest, ClaimRecoversStaleRowsLocksDueRowsAndUsesUniqueTokens) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  std::vector<std::string> sqls;
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    sqls.push_back(sql);
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    sqls.push_back(sql);
    return std::optional<QueryResult>{QueryResult{.rows = {{"hash-a", "2"}, {"hash-b", "0"}}}};
  };

  const auto result = mp.pool->claim_pending_chunk_deletions(2, std::chrono::system_clock::now());

  ASSERT_EQ(result.status, MutationStatus::OK);
  ASSERT_EQ(result.value->size(), 2U);
  EXPECT_FALSE((*result.value)[0].claim_token.empty());
  EXPECT_NE((*result.value)[0].claim_token, (*result.value)[1].claim_token);
  EXPECT_TRUE(std::ranges::any_of(sqls, [](const auto& sql) { return sql.find("SKIP LOCKED") != std::string::npos; }));
  EXPECT_TRUE(
    std::ranges::any_of(sqls, [](const auto& sql) { return sql.find("claimed_at < ?") != std::string::npos; }));
}

TEST(DatabasePoolPendingDeletionTest, CompleteAndReleaseRequireMatchingTokenAndCapBackoffAndError) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  std::vector<std::string> sqls;
  std::vector<std::vector<std::string>> params;
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>& values) {
    sqls.push_back(sql);
    params.push_back(values);
    return std::optional<int64_t>{1};
  };

  EXPECT_EQ(mp.pool->complete_pending_chunk_deletion("hash", "token").status, MutationStatus::OK);
  EXPECT_EQ(mp.pool->release_pending_chunk_deletion("hash", "token", std::string(700, 'x')).status, MutationStatus::OK);
  ASSERT_GE(params.size(), 2U);
  ASSERT_GE(sqls.size(), 2U);
  EXPECT_EQ(params[0], (std::vector<std::string>{"hash", "token"}));
  EXPECT_EQ(params[1][0].size(), 512U);
  EXPECT_EQ(params[1][1], "hash");
  EXPECT_EQ(params[1][2], "token");
  const auto next_attempt = sqls[1].find("next_attempt_at = DATE_ADD(UTC_TIMESTAMP(6), INTERVAL "
                                         "LEAST(POW(2, IF(retry_count >= 12, 12, retry_count + 1)), 3600) SECOND)");
  const auto retry_count = sqls[1].find("retry_count = IF(retry_count >= 2147483647, 2147483647, retry_count + 1)");
  ASSERT_NE(next_attempt, std::string::npos);
  ASSERT_NE(retry_count, std::string::npos);
  EXPECT_LT(next_attempt, retry_count);
}

TEST(DatabasePoolPendingDeletionTest, ThrowingPendingOperationsCloseAndReturnConnectionCapacity) {
  constexpr int kAttempts = 3;
  const auto expect_reusable_after = [kAttempts](auto configure, auto invoke, auto expected_status) {
    MockPool mp(1);
    auto* connection = mp.connections[0];
    connection->close_hook = [connection]() { connection->is_open_result = false; };
    connection->connect_hook = [connection]() { connection->is_open_result = true; };
    configure(*connection);

    for (int attempt = 0; attempt < kAttempts; ++attempt) {
      EXPECT_EQ(invoke(*mp.pool).status, expected_status) << attempt;
      EXPECT_TRUE(mp.pool->with_connection([](IConnection&) { return true; })) << attempt;
    }
    EXPECT_EQ(connection->close_count, kAttempts);
  };

  expect_reusable_after(
    [](MockConnection& connection) {
      connection.execute_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<int64_t> {
        return sql == "ROLLBACK" ? std::optional<int64_t>{} : std::optional<int64_t>{1};
      };
      connection.query_hook = [](const std::string&, const std::vector<std::string>&) -> std::optional<QueryResult> {
        throw std::runtime_error("claim query failed");
      };
    },
    [](DatabasePool& pool) { return pool.claim_pending_chunk_deletions(1, std::chrono::system_clock::now()); },
    MutationStatus::STORAGE_ERROR);

  expect_reusable_after(
    [](MockConnection& connection) {
      connection.execute_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<int64_t> {
        if (sql == "ROLLBACK") {
          return std::nullopt;
        }
        if (sql.starts_with("DELETE FROM pending_chunk_deletions")) {
          throw std::runtime_error("complete execute failed");
        }
        return 1;
      };
    },
    [](DatabasePool& pool) { return pool.complete_pending_chunk_deletion("hash", "token"); },
    MutationStatus::STORAGE_ERROR);

  expect_reusable_after(
    [](MockConnection& connection) {
      connection.execute_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<int64_t> {
        if (sql == "ROLLBACK") {
          return std::nullopt;
        }
        if (sql.starts_with("UPDATE pending_chunk_deletions")) {
          throw std::runtime_error("release execute failed");
        }
        return 1;
      };
    },
    [](DatabasePool& pool) { return pool.release_pending_chunk_deletion("hash", "token", "error"); },
    MutationStatus::STORAGE_ERROR);

  expect_reusable_after(
    [](MockConnection& connection) {
      connection.execute_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<int64_t> {
        return sql == "ROLLBACK" ? std::optional<int64_t>{} : std::optional<int64_t>{0};
      };
      connection.query_hook = [](const std::string&, const std::vector<std::string>&) -> std::optional<QueryResult> {
        throw std::runtime_error("reference query failed");
      };
    },
    [](DatabasePool& pool) { return pool.has_chunk_references("hash"); },
    LookupStatus::STORAGE_ERROR);

  expect_reusable_after(
    [](MockConnection& connection) {
      connection.execute_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<int64_t> {
        if (sql == "ROLLBACK") {
          return std::nullopt;
        }
        if (sql.starts_with("DELETE FROM pending_chunk_deletions")) {
          throw std::runtime_error("cancel execute failed");
        }
        return 1;
      };
    },
    [](DatabasePool& pool) { return pool.cancel_pending_chunk_deletion("hash", "token"); },
    MutationStatus::STORAGE_ERROR);
}

TEST(DatabasePoolPendingDeletionTest, MalformedDueListReturnsInvalidStateAndRollsBack) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  std::vector<std::string> executed;
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    executed.push_back(sql);
    return std::optional<int64_t>{1};
  };
  connection->query_result = QueryResult{.rows = {{"hash-only"}}};

  const auto result = mp.pool->claim_pending_chunk_deletions(2, std::chrono::system_clock::now());

  EXPECT_EQ(result.status, MutationStatus::INVALID_STATE);
  EXPECT_EQ(result.detail, "PENDING_STATE_INVALID");
  EXPECT_TRUE(std::ranges::any_of(executed, [](const auto& sql) { return sql == "ROLLBACK"; }));
  EXPECT_FALSE(std::ranges::any_of(executed, [](const auto& sql) { return sql == "COMMIT"; }));
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

TEST(DatabasePoolTest, ClosedConnectionReconnectsWithoutPingBeforeReuse) {
  MockConnection* connection = nullptr;
  std::vector<std::string> session_statements;
  auto factory = [&connection, &session_statements]() -> std::unique_ptr<IConnection> {
    auto mock = std::make_unique<MockConnection>();
    connection = mock.get();
    mock->execute_hook = [&session_statements](const std::string& sql,
                                               const std::vector<std::string>&) -> std::optional<int64_t> {
      session_statements.push_back(sql);
      return 0;
    };
    mock->connect_hook = [raw = mock.get()]() { configure_mysql_utc_session(*raw); };
    return mock;
  };
  DatabasePool pool(factory);
  DbConfig config;
  config.pool_size = 1;
  ASSERT_TRUE(pool.init(config));
  ASSERT_NE(connection, nullptr);
  EXPECT_EQ(session_statements, std::vector<std::string>{"SET time_zone = '+00:00'"});

  connection->is_open_result = false;
  connection->query_result = QueryResult{};

  EXPECT_FALSE(pool.get_user(1).has_value());
  EXPECT_EQ(connection->ping_count, 0);
  EXPECT_EQ(connection->close_count, 0);
  EXPECT_EQ(connection->connect_count, 2);
  EXPECT_EQ(session_statements, (std::vector<std::string>{"SET time_zone = '+00:00'", "SET time_zone = '+00:00'"}));
}

TEST(DatabasePoolTest, WithConnectionRollsBackWhenOperationReturnsFalse) {
  MockPool mp(1);
  int rollback_count = 0;
  mp.connections[0]->execute_hook = [&rollback_count](const std::string& sql,
                                                      const std::vector<std::string>&) -> std::optional<int64_t> {
    if (sql == "ROLLBACK") {
      ++rollback_count;
    }
    return 0;
  };

  EXPECT_FALSE(mp.pool->with_connection([](IConnection&) { return false; }));
  EXPECT_EQ(rollback_count, 1);
  EXPECT_EQ(mp.connections[0]->close_count, 0);
  EXPECT_TRUE(mp.pool->with_connection([](IConnection&) { return true; }));
}

TEST(DatabasePoolTest, WithConnectionRollsBackWhenOperationThrows) {
  MockPool mp(1);
  int rollback_count = 0;
  mp.connections[0]->execute_hook = [&rollback_count](const std::string& sql,
                                                      const std::vector<std::string>&) -> std::optional<int64_t> {
    if (sql == "ROLLBACK") {
      ++rollback_count;
    }
    return 0;
  };

  EXPECT_FALSE(mp.pool->with_connection([](IConnection&) -> bool { throw std::runtime_error("operation failed"); }));
  EXPECT_EQ(rollback_count, 1);
  EXPECT_EQ(mp.connections[0]->close_count, 0);
  EXPECT_TRUE(mp.pool->with_connection([](IConnection&) { return true; }));
}

TEST(DatabasePoolTest, WithConnectionClosesOnRollbackFailureAndReconnectsBeforeReuse) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  connection->execute_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<int64_t> {
    if (sql == "ROLLBACK") {
      return std::nullopt;
    }
    return 0;
  };
  connection->close_hook = [connection]() { connection->is_open_result = false; };
  connection->connect_hook = [connection]() { connection->is_open_result = true; };

  EXPECT_FALSE(mp.pool->with_connection([](IConnection&) { return false; }));
  EXPECT_EQ(connection->close_count, 1);
  EXPECT_EQ(connection->connect_count, 1);

  EXPECT_TRUE(mp.pool->with_connection([](IConnection&) { return true; }));
  EXPECT_EQ(connection->connect_count, 2);
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
  qr.columns = {"user_id", "username", "password_hash", "salt", "role", "email", "vip_expires_at", "created_at"};
  qr.rows.push_back({"42", "bob", "hash123", "somesalt", "1", "bob@test.com", "", "2024-01-15"});
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

TEST(DatabasePoolTest, GetUserRejectsTruncatedRow) {
  MockPool mp(1);
  QueryResult result;
  result.rows.push_back({"42", "bob", "hash123", "somesalt", "1", "bob@test.com", "2024-01-15"});
  mp.connections[0]->query_result = std::move(result);

  EXPECT_FALSE(mp.pool->get_user(42).has_value());
}

TEST(DatabasePoolTest, GetUserRejectsOversizedRow) {
  MockPool mp(1);
  QueryResult result;
  result.rows.push_back({"42", "bob", "hash123", "somesalt", "1", "bob@test.com", "", "2024-01-15", "extra"});
  mp.connections[0]->query_result = std::move(result);

  EXPECT_FALSE(mp.pool->get_user(42).has_value());
}

TEST(DatabasePoolTest, GetUserResultDistinguishesLookupFailures) {
  MockPool mp(1);

  mp.connections[0]->query_result = std::nullopt;
  EXPECT_EQ(mp.pool->get_user_result(42).status, LookupStatus::STORAGE_ERROR);

  mp.connections[0]->query_result = QueryResult{};
  EXPECT_EQ(mp.pool->get_user_result(42).status, LookupStatus::NOT_FOUND);

  QueryResult truncated;
  truncated.rows.push_back({"42", "bob"});
  mp.connections[0]->query_result = std::move(truncated);
  EXPECT_EQ(mp.pool->get_user_result(42).status, LookupStatus::INVALID_DATA);

  QueryResult malformed;
  malformed.rows.push_back({"invalid", "bob", "hash", "salt", "1", "bob@example.com", "", "2024-01-15"});
  mp.connections[0]->query_result = std::move(malformed);
  EXPECT_EQ(mp.pool->get_user_result(42).status, LookupStatus::INVALID_DATA);

  mp.pool->close();
  EXPECT_EQ(mp.pool->get_user_result(42).status, LookupStatus::STORAGE_ERROR);
}

TEST(DatabasePoolTest, GetUserResultReturnsFoundValueAndOptionalWrapper) {
  MockPool mp(1);
  QueryResult result;
  result.rows.push_back({"42", "bob", "hash", "salt", "1", "bob@example.com", "", "2024-01-15"});
  mp.connections[0]->query_result = result;

  const auto lookup = mp.pool->get_user_result(42);

  ASSERT_EQ(lookup.status, LookupStatus::FOUND);
  ASSERT_TRUE(lookup.value.has_value());
  EXPECT_EQ(lookup.value->user_id, 42);
  mp.connections[0]->query_result = std::move(result);
  EXPECT_TRUE(mp.pool->get_user(42).has_value());
}

TEST(DatabasePoolTest, GetUserResultRejectsEveryContradictoryRoleExpiryState) {
  const std::array invalid_states = {
    std::pair{UserRole::GUEST, std::string{"2034-01-01 00:00:00.000000"}},
    std::pair{UserRole::NORMAL, std::string{"2034-01-01 00:00:00.000000"}},
    std::pair{UserRole::ADMIN, std::string{"2034-01-01 00:00:00.000000"}},
    std::pair{UserRole::VIP, std::string{}},
  };

  for (const auto& [role, expires_at] : invalid_states) {
    MockPool mp(1);
    mp.connections[0]->query_result = QueryResult{.rows = {{"42",
                                                            "broken",
                                                            "hash",
                                                            "salt",
                                                            std::to_string(static_cast<int>(role)),
                                                            "broken@example.com",
                                                            expires_at,
                                                            "2026-01-02 03:04:05.000000"}}};

    const auto result = mp.pool->get_user_result(42);

    EXPECT_EQ(result.status, LookupStatus::INVALID_DATA) << static_cast<int>(role);
    EXPECT_FALSE(result.value.has_value());
  }
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

  EXPECT_EQ(mp.pool->create_user(u).status, MutationStatus::OK);

  // 验证 SQL 中含有参数化占位符
  EXPECT_NE(mp.connections[0]->last_sql.find("NULLIF(?, '')"), std::string::npos);
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

  EXPECT_EQ(mp.pool->create_user(u).status, MutationStatus::STORAGE_ERROR);
}

TEST(DatabasePoolTest, CreateUserStoresEmptyEmailAsNull) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;
  User user;
  user.username = "no_email";
  user.password_hash = "hash";

  EXPECT_EQ(mp.pool->create_user(user).status, MutationStatus::OK);
  EXPECT_NE(mp.connections[0]->last_sql.find("NULLIF(?, '')"), std::string::npos);
  EXPECT_TRUE(mp.connections[0]->last_params.back().empty());
}

TEST(DatabasePoolTest, CreateUserRejectsEmailLongerThan128BeforeWriting) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;
  User user;
  user.username = "long_email";
  user.password_hash = "hash";
  user.email = std::string(129, 'a');

  const auto creation = mp.pool->create_user(user);

  EXPECT_EQ(creation.status, MutationStatus::INVALID_STATE);
  EXPECT_EQ(creation.detail, "EMAIL_INVALID");
  EXPECT_TRUE(mp.connections[0]->last_sql.empty());
}

TEST(DatabasePoolTest, CreateUserRechecksUniqueKeysAfterConcurrentInsertFailure) {
  MockPool mp(1);
  mp.connections[0]->execute_result = std::nullopt;
  User user;
  user.username = "racing_user";
  user.password_hash = "hash";
  user.email = "racing@example.com";
  bool email_conflict = false;
  mp.connections[0]->query_hook = [&email_conflict](const std::string& sql,
                                                    const std::vector<std::string>&) -> std::optional<QueryResult> {
    QueryResult result;
    if (email_conflict && sql.find("email = ?") != std::string::npos) {
      result.rows.push_back({"9", "winner", "hash", "salt", "1", "racing@example.com", "", "2026-01-01"});
    }
    return result;
  };

  email_conflict = true;
  const auto email_result = mp.pool->create_user(user);
  EXPECT_EQ(email_result.status, MutationStatus::CONFLICT);
  EXPECT_EQ(email_result.detail, "EMAIL_CONFLICT");
  EXPECT_EQ(email_result.detail->find(user.email), std::string::npos);

  email_conflict = false;
  mp.connections[0]->query_hook = [](const std::string& sql,
                                     const std::vector<std::string>&) -> std::optional<QueryResult> {
    QueryResult result;
    if (sql.find("username = ?") != std::string::npos) {
      result.rows.push_back({"10", "racing_user", "hash", "salt", "1", "", "", "2026-01-01"});
    }
    return result;
  };
  const auto username_result = mp.pool->create_user(user);
  EXPECT_EQ(username_result.status, MutationStatus::CONFLICT);
  EXPECT_EQ(username_result.detail, "USERNAME_CONFLICT");
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
  EXPECT_NE(mp.connections[0]->last_sql.find("VALUES (?, ?, ?, ?, ?, NULLIF(?, '0'), ?)"), std::string::npos);
  EXPECT_NE(mp.connections[0]->last_sql.find("LAST_INSERT_ID(file_id)"), std::string::npos);
  ASSERT_EQ(mp.connections[0]->last_params.size(), 7U);
  EXPECT_EQ(mp.connections[0]->last_params[0], "record_test.bin");
  EXPECT_EQ(mp.connections[0]->last_params[1], "testhash123");
  EXPECT_EQ(mp.connections[0]->last_params[2], "512000");
  EXPECT_EQ(mp.connections[0]->last_params[3], "application/octet-stream");
  EXPECT_EQ(mp.connections[0]->last_params[4], "2097152");
  EXPECT_EQ(mp.connections[0]->last_params[5], "0");
  EXPECT_EQ(mp.connections[0]->last_params[6], "0");

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
  qr.rows.push_back({"1",
                     "record_test.bin",
                     "testhash123",
                     "512000",
                     "application/octet-stream",
                     "2097152",
                     "2024-07-01 00:00:00.000000",
                     "0",
                     "0"});
  mp.connections[0]->query_result = std::move(qr);

  auto result = mp.pool->get_file_record(1);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->file_name, "record_test.bin");
  EXPECT_EQ(result->file_hash, "testhash123");
  EXPECT_EQ(result->file_size, 512000U);
}

TEST(DatabasePoolFileDetailTest, LookupResultDistinguishesMissingStorageAndInvalidRows) {
  MockPool mp(1);
  auto* connection = mp.connections[0];

  connection->query_result = QueryResult{};
  auto result = mp.pool->get_file_record_result(7);
  EXPECT_EQ(result.status, LookupStatus::NOT_FOUND);
  EXPECT_FALSE(result.value.has_value());

  connection->query_result = std::nullopt;
  result = mp.pool->get_file_record_result(7);
  EXPECT_EQ(result.status, LookupStatus::STORAGE_ERROR);
  EXPECT_FALSE(result.value.has_value());

  connection->query_result = QueryResult{.rows = {{"7", "truncated"}}};
  result = mp.pool->get_file_record_result(7);
  EXPECT_EQ(result.status, LookupStatus::INVALID_DATA);
  EXPECT_FALSE(result.value.has_value());

  connection->query_result =
    QueryResult{.rows = {{"7", "detail.mp3", "hash-7", "1024", "audio/mpeg", "2097152", "not-a-datetime", "0", "42"}}};
  result = mp.pool->get_file_record_result(7);
  EXPECT_EQ(result.status, LookupStatus::INVALID_DATA);
  EXPECT_FALSE(result.value.has_value());

  connection->query_result = QueryResult{
    .rows = {{"7", "detail.mp3", "hash-7", "1024", "audio/mpeg", "2097152", "2026-01-03 00:00:00.000000", "0", "42"}}};
  result = mp.pool->get_file_record_result(7);
  ASSERT_EQ(result.status, LookupStatus::FOUND);
  ASSERT_TRUE(result.value.has_value());
  EXPECT_EQ(result.value->file_id, 7);
  EXPECT_EQ(result.value->created_at, "2026-01-03 00:00:00.000000");
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
  qr.rows.push_back({"2",
                     "hash_lookup.bin",
                     "byhash789",
                     "256000",
                     "application/octet-stream",
                     "2097152",
                     "2024-07-02 00:00:00.000000",
                     "0",
                     "0"});
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
  EXPECT_EQ(mp.connections[0]->last_insert_id_count, 1);
  EXPECT_NE(mp.connections[0]->last_sql.find("ON DUPLICATE KEY UPDATE file_id=LAST_INSERT_ID(file_id)"),
            std::string::npos);
  ASSERT_EQ(mp.connections[0]->last_params.size(), 7U);
  EXPECT_EQ(mp.connections[0]->last_params[1], "existing_hash");
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

TEST(DatabasePoolTest, GetAuthUserResultDistinguishesLookupFailures) {
  MockPool mp(1);

  mp.connections[0]->query_result = std::nullopt;
  EXPECT_EQ(mp.pool->get_auth_user_result("testuser").status, LookupStatus::STORAGE_ERROR);

  mp.connections[0]->query_result = QueryResult{};
  EXPECT_EQ(mp.pool->get_auth_user_result("testuser").status, LookupStatus::NOT_FOUND);

  QueryResult truncated;
  truncated.rows.push_back({"10", "testuser"});
  mp.connections[0]->query_result = std::move(truncated);
  EXPECT_EQ(mp.pool->get_auth_user_result("testuser").status, LookupStatus::INVALID_DATA);

  QueryResult malformed;
  malformed.rows.push_back({"not-an-id", "testuser", "1"});
  mp.connections[0]->query_result = std::move(malformed);
  EXPECT_EQ(mp.pool->get_auth_user_result("testuser").status, LookupStatus::INVALID_DATA);
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
  auto* connection = mp.connections[0];
  std::vector<std::string> sqls;
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    sqls.push_back(sql);
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    sqls.push_back(sql);
    return std::optional<QueryResult>{QueryResult{
      .columns = {"file_id",
                  "file_name",
                  "file_hash",
                  "file_size",
                  "content_type",
                  "chunk_size",
                  "created_at",
                  "music_id",
                  "uploaded_by",
                  "total"},
      .rows = {
        {"1", "song.mp3", "hash1", "1000", "audio/mpeg", "2097152", "2024-01-01 00:00:00.000000", "1", "1", "1"}}}};
  };

  int total = 0;
  auto results = mp.pool->search_files_ext("song", "audio", 0, 10, total);
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(total, 1);
  EXPECT_EQ(results[0].file_name, "song.mp3");
  EXPECT_EQ(results[0].music_id, 1);
  ASSERT_EQ(sqls.size(), 1U);
  EXPECT_NE(sqls.front().find("COUNT(*) OVER()"), std::string::npos);
}

TEST(DatabasePoolFileListTest, UsesOneWindowQueryAndStrictRows) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  std::vector<std::string> sqls;
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    sqls.push_back(sql);
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    sqls.push_back(sql);
    return std::optional<QueryResult>{QueryResult{
      .rows = {{"7", "file", "hash", "10", "image/png", "4", "2026-01-01 00:00:00.000000", "0", "42", "1"}}}};
  };

  const auto result = mp.pool->list_files("file", "image", 0, 20);

  ASSERT_EQ(result.status, LookupStatus::FOUND);
  ASSERT_TRUE(result.value);
  EXPECT_EQ(result.value->total, 1);
  ASSERT_EQ(result.value->items.size(), 1U);
  EXPECT_EQ(result.value->items[0].uploaded_by, 42);
  ASSERT_EQ(sqls.size(), 1U);
  EXPECT_NE(sqls.front().find("COUNT(*) OVER()"), std::string::npos);
}

TEST(DatabasePoolFileListTest, EmptyPageUsesSingleQuerySentinelToPreserveTotal) {
  MockPool mp(1);
  std::vector<std::pair<std::string, std::vector<std::string>>> queries;
  mp.connections[0]->query_hook = [&](const std::string& sql, const std::vector<std::string>& params) {
    queries.emplace_back(sql, params);
    return std::optional<QueryResult>{QueryResult{.rows = {{"", "", "", "", "", "", "", "", "", "4"}}}};
  };

  const auto result = mp.pool->list_files("file", "image", 40, 20);

  ASSERT_EQ(result.status, LookupStatus::FOUND);
  ASSERT_TRUE(result.value);
  EXPECT_EQ(result.value->total, 4);
  EXPECT_TRUE(result.value->items.empty());
  ASSERT_EQ(queries.size(), 1U);
  EXPECT_EQ(queries[0].second, (std::vector<std::string>{"%file%", "image", "20", "40"}));
  EXPECT_NE(queries[0].first.find("LEFT JOIN paged_files"), std::string::npos);
}

TEST(DatabasePoolFileListTest, OtherUsesParameterizedTopLevelExclusionsForCountAndItems) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  std::vector<std::pair<std::string, std::vector<std::string>>> queries;
  connection->execute_hook = [](const std::string&, const std::vector<std::string>&) {
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [&](const std::string& sql, const std::vector<std::string>& params) {
    queries.emplace_back(sql, params);
    return std::optional<QueryResult>{QueryResult{
      .rows = {
        {"7", "report.pdf", "hash", "10", "application/pdf", "4", "2026-01-01 00:00:00.000000", "", "42", "1"}}}};
  };

  const auto result = mp.pool->list_files("report", "other", 0, 20);

  ASSERT_EQ(result.status, LookupStatus::FOUND);
  ASSERT_TRUE(result.value);
  ASSERT_EQ(result.value->total, 1);
  ASSERT_EQ(result.value->items.size(), 1U);
  ASSERT_EQ(queries.size(), 1U);
  const std::string predicate = "f.content_type NOT LIKE CONCAT(?, '/%')";
  EXPECT_EQ(std::ranges::count(queries[0].first, '?'), 6);
  EXPECT_NE(queries[0].first.find(predicate), std::string::npos);
  EXPECT_NE(queries[0].first.find("COUNT(*) OVER()"), std::string::npos);
  EXPECT_EQ(queries[0].second, (std::vector<std::string>{"%report%", "audio", "image", "video", "20", "0"}));
}

TEST(DatabasePoolFileListTest, RejectsUnknownTypeBeforeOpeningTransaction) {
  MockPool mp(1);
  std::vector<std::string> executed;
  mp.connections[0]->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    executed.push_back(sql);
    return std::optional<int64_t>{1};
  };

  const auto result = mp.pool->list_files("", "application/pdf", 0, 20);

  EXPECT_EQ(result.status, LookupStatus::INVALID_DATA);
  EXPECT_TRUE(executed.empty());
}

TEST(DatabasePoolFileListTest, RejectsMalformedRows) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  std::vector<std::string> executed;
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    executed.push_back(sql);
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) {
    static_cast<void>(sql);
    return std::optional<QueryResult>{QueryResult{.rows = {{"truncated"}}}};
  };

  const auto result = mp.pool->list_files("", "", 0, 20);

  EXPECT_EQ(result.status, LookupStatus::INVALID_DATA);
  EXPECT_TRUE(executed.empty());
}

TEST(DatabasePoolFileListTest, RejectsInvalidCreatedAt) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  std::vector<std::string> executed;
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    executed.push_back(sql);
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) {
    static_cast<void>(sql);
    return std::optional<QueryResult>{
      QueryResult{.rows = {{"7", "file", "hash", "10", "image/png", "4", "not-a-datetime", "0", "42", "1"}}}};
  };

  const auto result = mp.pool->list_files("", "", 0, 20);

  EXPECT_EQ(result.status, LookupStatus::INVALID_DATA);
  EXPECT_TRUE(executed.empty());
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
  EXPECT_NE(mp.connections[0]->last_sql.find("music_id = NULLIF(?, '0')"), std::string::npos);
  ASSERT_EQ(mp.connections[0]->last_params.size(), 5U);
  EXPECT_EQ(mp.connections[0]->last_params[0], "updated.mp3");
  EXPECT_EQ(mp.connections[0]->last_params[1], "audio/mpeg");
  EXPECT_EQ(mp.connections[0]->last_params[2], "5");
  EXPECT_EQ(mp.connections[0]->last_params[3], "1");
  EXPECT_EQ(mp.connections[0]->last_params[4], "1");
}

// ============================================================
// T21: update_user
// ============================================================
TEST(DatabasePoolTest, UpdateUser) {
  MockPool mp(1);
  std::vector<std::pair<std::string, std::vector<std::string>>> executions;
  mp.connections[0]->execute_hook = [&executions](const std::string& sql, const std::vector<std::string>& params) {
    executions.emplace_back(sql, params);
    return std::optional<int64_t>{1};
  };

  User u;
  u.user_id = 1;
  u.email = "new@test.com";
  u.password_hash = "new_hash";
  u.salt = "new_salt";

  EXPECT_EQ(mp.pool->update_user(u).status, MutationStatus::OK);
  ASSERT_EQ(executions.size(), 3U);
  EXPECT_EQ(executions[0].first, "START TRANSACTION");
  EXPECT_EQ(executions[2].first, "COMMIT");
  EXPECT_NE(executions[1].first.find("UPDATE users SET email = ?"), std::string::npos);
  EXPECT_NE(executions[1].first.find("password_hash = ?"), std::string::npos);
  EXPECT_NE(executions[1].first.find("salt = ?"), std::string::npos);
  EXPECT_EQ(executions[1].second, (std::vector<std::string>{"new@test.com", "new_hash", "new_salt", "1"}));
}

TEST(DatabasePoolTest, UpdateUserMapsConcurrentEmailConflictWithoutSensitiveDetail) {
  MockPool mp(1);
  mp.connections[0]->execute_hook = [](const std::string& sql, const std::vector<std::string>&) {
    if (sql == "START TRANSACTION") {
      return std::optional<int64_t>{1};
    }
    return std::optional<int64_t>{std::nullopt};
  };
  User user;
  user.user_id = 1;
  user.email = "occupied@example.com";
  user.password_hash = "hash";
  user.salt = "salt";
  mp.connections[0]->query_hook = [&user](const std::string& sql,
                                          const std::vector<std::string>&) -> std::optional<QueryResult> {
    QueryResult result;
    if (sql.find("email = ?") != std::string::npos) {
      result.rows.push_back({"2", "other", "hash", "salt", "1", user.email, "", "2026-01-01"});
    }
    return result;
  };

  const auto update = mp.pool->update_user(user);

  EXPECT_EQ(update.status, MutationStatus::CONFLICT);
  EXPECT_EQ(update.detail, "EMAIL_CONFLICT");
  EXPECT_EQ(update.detail->find(user.email), std::string::npos);
}

TEST(DatabasePoolProfilePatchTest, ConcurrentPartialUpdatesPreserveBothFieldsAndCommit) {
  MockPool mp(2);

  struct StoredUser {
    std::string email{"old@example.com"};
    std::string password_hash{"old-hash"};
    std::string salt{"old-salt"};
  } stored;

  std::mutex stored_mutex;
  std::barrier synchronize_updates{2};
  std::atomic<int> transaction_starts{0};
  std::atomic<int> commits{0};

  for (auto* connection : mp.connections) {
    connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>& params) {
      if (sql == "START TRANSACTION") {
        transaction_starts.fetch_add(1, std::memory_order_relaxed);
        return std::optional<int64_t>{1};
      }
      if (sql == "COMMIT") {
        commits.fetch_add(1, std::memory_order_relaxed);
        return std::optional<int64_t>{1};
      }
      if (sql.starts_with("UPDATE users SET")) {
        synchronize_updates.arrive_and_wait();
        std::lock_guard lock(stored_mutex);
        std::size_t parameter_index = 0;
        if (sql.find("email = ?") != std::string::npos) {
          stored.email = params.at(parameter_index++);
        }
        if (sql.find("password_hash = ?") != std::string::npos) {
          stored.password_hash = params.at(parameter_index++);
          stored.salt = params.at(parameter_index++);
        }
        return std::optional<int64_t>{1};
      }
      return std::optional<int64_t>{1};
    };
  }

  User email_patch;
  email_patch.user_id = 1;
  email_patch.email = "new@example.com";
  User password_patch;
  password_patch.user_id = 1;
  password_patch.password_hash = "new-hash";
  password_patch.salt = "new-salt";

  auto email_result = std::async(std::launch::async, [&] { return mp.pool->update_user(email_patch); });
  auto password_result = std::async(std::launch::async, [&] { return mp.pool->update_user(password_patch); });

  EXPECT_EQ(email_result.get().status, MutationStatus::OK);
  EXPECT_EQ(password_result.get().status, MutationStatus::OK);
  EXPECT_EQ(stored.email, "new@example.com");
  EXPECT_EQ(stored.password_hash, "new-hash");
  EXPECT_EQ(stored.salt, "new-salt");
  EXPECT_EQ(transaction_starts.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(commits.load(std::memory_order_relaxed), 2);
}

TEST(DatabasePoolTest, UpdateUserRejectsEmailLongerThan128BeforeWriting) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;
  User user;
  user.user_id = 1;
  user.email = std::string(129, 'a');

  const auto update = mp.pool->update_user(user);

  EXPECT_EQ(update.status, MutationStatus::INVALID_STATE);
  EXPECT_EQ(update.detail, "EMAIL_INVALID");
  EXPECT_TRUE(mp.connections[0]->last_sql.empty());
}

TEST(DatabasePoolTest, ListMusicLibraryUsesOneWindowQueryWithoutInvalidGroupBy) {
  MockPool mp(1);
  std::vector<std::pair<std::string, std::vector<std::string>>> queries;
  mp.connections[0]->query_hook = [&](const std::string& sql,
                                      const std::vector<std::string>& params) -> std::optional<QueryResult> {
    queries.emplace_back(sql, params);
    QueryResult result;
    result.columns = {"music_id",
                      "title",
                      "artist",
                      "album",
                      "genre",
                      "duration_sec",
                      "track_number",
                      "created_at",
                      "updated_at",
                      "total"};
    result.rows.push_back({"7", "Song", "Artist", "Album", "Pop", "180", "2", "2024-01-01", "2024-01-02", "1"});
    return result;
  };

  int total = 0;
  auto items = mp.pool->list_music_library("song", 0, 10, total);

  EXPECT_EQ(total, 1);
  ASSERT_EQ(items.size(), 1U);
  EXPECT_EQ(items[0].music_id, 7);
  ASSERT_EQ(queries.size(), 1U);
  EXPECT_NE(queries[0].first.find("EXISTS"), std::string::npos);
  EXPECT_NE(queries[0].first.find("COUNT(*) OVER()"), std::string::npos);
  EXPECT_EQ(queries[0].first.find("GROUP BY"), std::string::npos);
  EXPECT_EQ(queries[0].first.find("file_hash"), std::string::npos);
  EXPECT_EQ(queries[0].second, (std::vector<std::string>{"%song%", "%song%", "%song%", "10", "0"}));
}

TEST(DatabasePoolTest, ListMusicLibraryEmptyPageUsesSingleQuerySentinelToPreserveTotal) {
  MockPool mp(1);
  std::vector<std::pair<std::string, std::vector<std::string>>> queries;
  mp.connections[0]->query_hook = [&](const std::string& sql, const std::vector<std::string>& params) {
    queries.emplace_back(sql, params);
    return std::optional<QueryResult>{QueryResult{.rows = {{"", "", "", "", "", "", "", "", "", "3"}}}};
  };

  int total = 0;
  const auto items = mp.pool->list_music_library("song", 20, 10, total);

  EXPECT_TRUE(items.empty());
  EXPECT_EQ(total, 3);
  ASSERT_EQ(queries.size(), 1U);
  EXPECT_EQ(queries[0].second, (std::vector<std::string>{"%song%", "%song%", "%song%", "10", "20"}));
  EXPECT_NE(queries[0].first.find("LEFT JOIN paged_music"), std::string::npos);
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

  auto playlists = mp.pool->get_user_playlists(1, 1);
  ASSERT_EQ(playlists.status, MutationStatus::OK);
  ASSERT_TRUE(playlists.value.has_value());
  ASSERT_EQ(playlists.value->size(), 1U);
  EXPECT_EQ(playlists.value->at(0).name, "Favorites");
  EXPECT_EQ(playlists.value->at(0).item_count, 3);
}

// ============================================================
// T27: create_playlist + last_insert_id
// ============================================================
TEST(DatabasePoolTest, CreatePlaylist) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;
  mp.connections[0]->last_insert_id_value = 10;
  mp.connections[0]->query_hook = [](const std::string& sql,
                                     const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("FROM users") != std::string::npos)
      return QueryResult{.rows = {{"1"}}};
    return QueryResult{.rows = {{"10", "1", "My Playlist", "", "0", "2026-07-28"}}};
  };

  Playlist pl;
  pl.user_id = 1;
  pl.name = "My Playlist";

  auto created = mp.pool->create_playlist(pl, 1);
  ASSERT_EQ(created.status, MutationStatus::OK);
  ASSERT_TRUE(created.value.has_value());
  EXPECT_EQ(created.value->playlist_id, 10);
}

// ============================================================
// T28: add_playlist_item + remove_playlist_item + reorder
// ============================================================
TEST(DatabasePoolTest, AddRemoveReorderPlaylistItems) {
  MockPool mp(1);

  int phase = 0;
  mp.connections[0]->query_hook = [&phase](const std::string& sql,
                                           const std::vector<std::string>& params) -> std::optional<QueryResult> {
    if (sql.find("user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"1"}}};
    if (sql.find("music_meta") != std::string::npos)
      return QueryResult{.rows = {{params.at(0)}}};
    if (phase == 0)
      return QueryResult{};
    if (phase == 1)
      return QueryResult{.rows = {{"5", "0"}}};
    return QueryResult{.rows = {{"3", "0"}, {"1", "1"}, {"2", "2"}}};
  };
  mp.connections[0]->execute_result = 1;

  EXPECT_EQ(mp.pool->add_playlist_item(1, 1, 5).status, MutationStatus::OK);

  phase = 1;
  EXPECT_EQ(mp.pool->remove_playlist_item(1, 1, 5).status, MutationStatus::OK);

  phase = 2;
  EXPECT_EQ(mp.pool->reorder_playlist_items(1, 1, {3, 1, 2}).status, MutationStatus::OK);
}

// ============================================================
// T29: get_playlist_items (STRAIGHT_JOIN)
// ============================================================
TEST(DatabasePoolTest, GetPlaylistItems) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;

  QueryResult qr;
  qr.columns = {"id", "playlist_id", "music_id", "sort_order", "added_at", "title", "artist", "file_hash"};
  qr.rows.push_back({"1", "1", "5", "0", "2024-01-01", "Song", "Artist", "abcdef"});
  mp.connections[0]->query_hook = [qr = std::move(qr)](const std::string& sql,
                                                       const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"1"}}};
    return qr;
  };

  auto items = mp.pool->get_playlist_items(1, 1);
  ASSERT_EQ(items.status, MutationStatus::OK);
  ASSERT_TRUE(items.value.has_value());
  ASSERT_EQ(items.value->size(), 1U);
  EXPECT_EQ(items.value->at(0).title, "Song");
  EXPECT_EQ(items.value->at(0).artist, "Artist");
  EXPECT_EQ(items.value->at(0).file_hash, "abcdef");
  EXPECT_EQ(items.value->at(0).sort_order, 0);
}

TEST(DatabasePoolPlaylistTest, RemoveLocksOwnerThenItemsAndCompactsPositionsInOneTransaction) {
  MockPool mp(1);
  std::vector<std::string> operations;
  mp.connections[0]->query_hook = [&operations](const std::string& sql,
                                                const std::vector<std::string>&) -> std::optional<QueryResult> {
    operations.push_back(sql);
    if (sql.find("FROM user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    return QueryResult{.rows = {{"3", "0"}, {"5", "1"}, {"9", "2"}}};
  };
  mp.connections[0]->execute_hook = [&operations](const std::string& sql,
                                                  const std::vector<std::string>&) -> std::optional<int64_t> {
    operations.push_back(sql);
    return 1;
  };

  const auto result = mp.pool->remove_playlist_item(7, 42, 5);

  EXPECT_EQ(result.status, MutationStatus::OK);
  ASSERT_GE(operations.size(), 6U);
  EXPECT_EQ(operations[0], "START TRANSACTION");
  EXPECT_NE(operations[1].find("user_playlists"), std::string::npos);
  EXPECT_NE(operations[1].find("FOR UPDATE"), std::string::npos);
  EXPECT_NE(operations[2].find("playlist_items"), std::string::npos);
  EXPECT_NE(operations[2].find("FOR UPDATE"), std::string::npos);
  EXPECT_EQ(operations[4], "UPDATE playlist_items SET sort_order = ? WHERE playlist_id = ? AND music_id = ?");
  EXPECT_EQ(operations[5], "UPDATE playlist_items SET sort_order = ? WHERE playlist_id = ? AND music_id = ?");
  EXPECT_EQ(operations.back(), "COMMIT");
}

TEST(DatabasePoolPlaylistTest, RemoveLastItemAcceptsUnchangedRemainingPositions) {
  MockPool mp(1);
  bool committed = false;
  mp.connections[0]->query_hook = [](const std::string& sql,
                                     const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("FROM user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    return QueryResult{.rows = {{"3", "0"}, {"5", "1"}, {"9", "2"}}};
  };
  mp.connections[0]->execute_hook = [&committed](const std::string& sql,
                                                 const std::vector<std::string>&) -> std::optional<int64_t> {
    if (sql.starts_with("UPDATE playlist_items"))
      return 0;
    if (sql == "COMMIT")
      committed = true;
    return 1;
  };

  const auto result = mp.pool->remove_playlist_item(7, 42, 9);

  EXPECT_EQ(result.status, MutationStatus::OK);
  EXPECT_TRUE(committed);
}

TEST(DatabasePoolPlaylistTest, ReorderRequiresExactUniqueSetIncludingEmptyCase) {
  const auto invoke = [](const std::vector<std::string>& existing, const std::vector<int64_t>& requested) {
    MockPool mp(1);
    int update_count = 0;
    mp.connections[0]->query_hook = [existing](const std::string& sql,
                                               const std::vector<std::string>&) -> std::optional<QueryResult> {
      if (sql.find("FROM user_playlists") != std::string::npos)
        return QueryResult{.rows = {{"42"}}};
      QueryResult result;
      for (std::size_t index = 0; index < existing.size(); ++index)
        result.rows.push_back({existing[index], std::to_string(index)});
      return result;
    };
    mp.connections[0]->execute_hook = [&update_count](const std::string& sql,
                                                      const std::vector<std::string>&) -> std::optional<int64_t> {
      if (sql.starts_with("UPDATE playlist_items"))
        ++update_count;
      return 1;
    };
    const auto result = mp.pool->reorder_playlist_items(7, 42, requested);
    return std::pair{result.status, update_count};
  };

  EXPECT_EQ(invoke({"3", "5"}, {3, 3}).first, MutationStatus::CONFLICT);
  EXPECT_EQ(invoke({"3", "5"}, {3}).first, MutationStatus::CONFLICT);
  EXPECT_EQ(invoke({"3", "5"}, {3, 5, 9}).first, MutationStatus::CONFLICT);
  EXPECT_EQ(invoke({"3", "5"}, {}).first, MutationStatus::CONFLICT);
  EXPECT_EQ(invoke({}, {}).first, MutationStatus::OK);
  const auto valid = invoke({"3", "5"}, {5, 3});
  EXPECT_EQ(valid.first, MutationStatus::OK);
  EXPECT_EQ(valid.second, 2);
}

TEST(DatabasePoolPlaylistTest, MutationFailureRollsBackWithoutCommit) {
  MockPool mp(1);
  int rollback_count = 0;
  int commit_count = 0;
  mp.connections[0]->query_hook = [](const std::string& sql,
                                     const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("FROM user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    return QueryResult{.rows = {{"3", "0"}, {"5", "1"}}};
  };
  mp.connections[0]->execute_hook = [&rollback_count,
                                     &commit_count](const std::string& sql,
                                                    const std::vector<std::string>& params) -> std::optional<int64_t> {
    if (sql == "ROLLBACK")
      ++rollback_count;
    if (sql == "COMMIT")
      ++commit_count;
    if (sql.starts_with("UPDATE playlist_items") && params.at(0) == "1")
      return std::nullopt;
    return 1;
  };

  const auto result = mp.pool->reorder_playlist_items(7, 42, {5, 3});

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  EXPECT_EQ(rollback_count, 1);
  EXPECT_EQ(commit_count, 0);
}

TEST(DatabasePoolPlaylistTest, StrictlyValidatesOwnerAndCreateUserLockRows) {
  const auto update_status = [](QueryResult owner) {
    MockPool mp(1);
    mp.connections[0]->query_result = std::move(owner);
    mp.connections[0]->execute_result = 1;
    return mp.pool->update_playlist(7, 42, "name", "description").status;
  };
  EXPECT_EQ(update_status(QueryResult{.rows = {{"42"}, {"42"}}}), MutationStatus::INVALID_STATE);
  EXPECT_EQ(update_status(QueryResult{.rows = {{"42", "extra"}}}), MutationStatus::INVALID_STATE);
  EXPECT_EQ(update_status(QueryResult{.rows = {{"invalid"}}}), MutationStatus::INVALID_STATE);
  EXPECT_EQ(update_status(QueryResult{.rows = {{"99"}}}), MutationStatus::OWNER_REQUIRED);

  for (const QueryResult& user : {QueryResult{.rows = {{"42"}, {"42"}}},
                                  QueryResult{.rows = {{"42", "extra"}}},
                                  QueryResult{.rows = {{"invalid"}}}}) {
    MockPool mp(1);
    mp.connections[0]->query_result = user;
    mp.connections[0]->execute_result = 1;
    mp.connections[0]->last_insert_id_value = 7;
    Playlist playlist;
    playlist.user_id = 42;
    playlist.name = "name";
    EXPECT_EQ(mp.pool->create_playlist(playlist, 42).status, MutationStatus::INVALID_STATE);
  }
}

TEST(DatabasePoolPlaylistTest, CreateDistinguishesMissingTargetUser) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;
  mp.connections[0]->query_result = QueryResult{};
  Playlist playlist;
  playlist.user_id = 42;
  playlist.name = "name";

  const auto result = mp.pool->create_playlist(playlist, 42);

  EXPECT_EQ(result.status, MutationStatus::USER_NOT_FOUND);
  EXPECT_EQ(result.detail, "USER_NOT_FOUND");
}

TEST(DatabasePoolPlaylistTest, CreateAndUpdateReturnCompletePersistedPlaylist) {
  const QueryResult complete{.rows = {{"7", "42", "Persisted", "Stored", "3", "2026-07-28 01:02:03"}}};
  {
    MockPool mp(1);
    mp.connections[0]->last_insert_id_value = 7;
    mp.connections[0]->execute_result = 1;
    mp.connections[0]->query_hook = [complete](const std::string& sql,
                                               const std::vector<std::string>&) -> std::optional<QueryResult> {
      if (sql.find("FROM users") != std::string::npos)
        return QueryResult{.rows = {{"42"}}};
      return complete;
    };
    Playlist playlist;
    playlist.user_id = 42;
    playlist.name = "Requested";
    playlist.description = "Input";

    const auto result = mp.pool->create_playlist(playlist, 42);

    ASSERT_EQ(result.status, MutationStatus::OK);
    ASSERT_TRUE(result.value.has_value());
    EXPECT_EQ(result.value->playlist_id, 7);
    EXPECT_EQ(result.value->user_id, 42);
    EXPECT_EQ(result.value->name, "Persisted");
    EXPECT_EQ(result.value->description, "Stored");
    EXPECT_EQ(result.value->item_count, 3);
    EXPECT_EQ(result.value->created_at, "2026-07-28 01:02:03");
  }
  {
    MockPool mp(1);
    mp.connections[0]->execute_result = 1;
    mp.connections[0]->query_hook = [complete](const std::string& sql,
                                               const std::vector<std::string>&) -> std::optional<QueryResult> {
      if (sql.find("SELECT user_id FROM user_playlists") != std::string::npos)
        return QueryResult{.rows = {{"42"}}};
      if (sql.find("SELECT music_id, sort_order") != std::string::npos)
        return QueryResult{};
      return complete;
    };

    const auto result = mp.pool->update_playlist(7, 42, "Requested", "Input");

    ASSERT_EQ(result.status, MutationStatus::OK);
    ASSERT_TRUE(result.value.has_value());
    EXPECT_EQ(result.value->item_count, 3);
    EXPECT_EQ(result.value->created_at, "2026-07-28 01:02:03");
    EXPECT_EQ(result.value->name, "Persisted");
  }
}

TEST(DatabasePoolPlaylistTest, ListRequiresExactlySixStrictColumns) {
  MockPool mp(1);
  mp.connections[0]->query_result = QueryResult{.rows = {{"7", "42", "name", "desc", "0", "now", "extra"}}};

  EXPECT_EQ(mp.pool->get_user_playlists(42, 42).status, MutationStatus::INVALID_STATE);
}

TEST(DatabasePoolPlaylistTest, GetItemsUsesOneSnapshotAndStrictOwnerRow) {
  MockPool mp(1);
  std::vector<std::string> operations;
  mp.connections[0]->execute_hook = [&operations](const std::string& sql,
                                                  const std::vector<std::string>&) -> std::optional<int64_t> {
    operations.push_back(sql);
    return 1;
  };
  mp.connections[0]->query_hook = [&operations](const std::string& sql,
                                                const std::vector<std::string>&) -> std::optional<QueryResult> {
    operations.push_back(sql);
    if (sql.find("user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    return QueryResult{.rows = {{"1", "7", "5", "0", "now", "song", "artist", "hash"}}};
  };

  const auto result = mp.pool->get_playlist_items(7, 42);

  ASSERT_EQ(result.status, MutationStatus::OK);
  EXPECT_EQ(operations.front(), "START TRANSACTION WITH CONSISTENT SNAPSHOT");
  EXPECT_NE(operations.at(1).find("user_playlists"), std::string::npos);
  EXPECT_NE(operations.at(2).find("playlist_items"), std::string::npos);
  EXPECT_EQ(operations.back(), "COMMIT");

  for (const QueryResult& owner : {QueryResult{.rows = {{"42"}, {"42"}}},
                                   QueryResult{.rows = {{"42", "extra"}}},
                                   QueryResult{.rows = {{"invalid"}}}}) {
    MockPool malformed(1);
    malformed.connections[0]->execute_result = 1;
    malformed.connections[0]->query_result = owner;
    EXPECT_EQ(malformed.pool->get_playlist_items(7, 42).status, MutationStatus::INVALID_STATE);
  }
}

TEST(DatabasePoolPlaylistTest, AddLocksMusicBeforeOwnerAndItemsAndStopsWhenMusicIsMissing) {
  MockPool mp(1);
  std::vector<std::string> queries;
  bool music_exists = true;
  mp.connections[0]->execute_result = 1;
  mp.connections[0]->query_hook =
    [&queries, &music_exists](const std::string& sql, const std::vector<std::string>&) -> std::optional<QueryResult> {
    queries.push_back(sql);
    if (sql.find("user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    if (sql.find("playlist_items") != std::string::npos)
      return QueryResult{};
    if (sql.find("music_meta") != std::string::npos)
      return music_exists ? QueryResult{.rows = {{"9"}}} : QueryResult{};
    return QueryResult{};
  };

  EXPECT_EQ(mp.pool->add_playlist_item(7, 42, 9).status, MutationStatus::OK);
  ASSERT_EQ(queries.size(), 3U);
  EXPECT_NE(queries[0].find("music_meta"), std::string::npos);
  EXPECT_NE(queries[0].find("FOR UPDATE"), std::string::npos);
  EXPECT_NE(queries[1].find("user_playlists"), std::string::npos);
  EXPECT_NE(queries[2].find("playlist_items"), std::string::npos);

  queries.clear();
  music_exists = false;
  const auto missing = mp.pool->add_playlist_item(7, 42, 99);
  EXPECT_EQ(missing.status, MutationStatus::NOT_FOUND);
  EXPECT_EQ(missing.detail, "MUSIC_NOT_FOUND");
  ASSERT_EQ(queries.size(), 1U);
  EXPECT_NE(queries[0].find("music_meta"), std::string::npos);
}

TEST(DatabasePoolPlaylistTest, AddLocksMusicBeforeReportingDuplicateConflict) {
  MockPool mp(1);
  std::vector<std::string> queries;
  mp.connections[0]->execute_result = 1;
  mp.connections[0]->query_hook = [&queries](const std::string& sql,
                                             const std::vector<std::string>&) -> std::optional<QueryResult> {
    queries.push_back(sql);
    if (sql.find("user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    if (sql.find("playlist_items") != std::string::npos)
      return QueryResult{.rows = {{"9", "0"}}};
    if (sql.find("music_meta") != std::string::npos)
      return QueryResult{.rows = {{"9"}}};
    return QueryResult{};
  };

  const auto result = mp.pool->add_playlist_item(7, 42, 9);

  EXPECT_EQ(result.status, MutationStatus::CONFLICT);
  ASSERT_EQ(queries.size(), 3U);
  EXPECT_NE(queries.front().find("music_meta"), std::string::npos);
}

TEST(DatabasePoolPlaylistTest, ConcurrentAddsReachSharedMusicLockBeforeAnyPlaylistLock) {
  MockPool mp(2);
  std::barrier music_barrier(2);
  std::atomic<int> music_locks{0};
  std::atomic<bool> playlist_lock_before_all_music{false};
  for (auto* connection : mp.connections) {
    connection->execute_result = 1;
    connection->query_hook = [&](const std::string& sql,
                                 const std::vector<std::string>&) -> std::optional<QueryResult> {
      if (sql.find("music_meta") != std::string::npos) {
        music_locks.fetch_add(1);
        music_barrier.arrive_and_wait();
        return QueryResult{.rows = {{"9"}}};
      }
      if (sql.find("user_playlists") != std::string::npos) {
        if (music_locks.load() != 2)
          playlist_lock_before_all_music.store(true);
        return QueryResult{.rows = {{"42"}}};
      }
      return QueryResult{};
    };
  }

  auto first = std::async(std::launch::async, [&] { return mp.pool->add_playlist_item(7, 42, 9); });
  auto second = std::async(std::launch::async, [&] { return mp.pool->add_playlist_item(8, 42, 9); });

  EXPECT_EQ(first.get().status, MutationStatus::OK);
  EXPECT_EQ(second.get().status, MutationStatus::OK);
  EXPECT_FALSE(playlist_lock_before_all_music.load());
}

TEST(DatabasePoolPlaylistTest, ItemMutationsStrictlyRejectMalformedLockedRows) {
  const std::vector<QueryResult> malformed = {
    QueryResult{.rows = {{"3"}}},
    QueryResult{.rows = {{"3", "0", "extra"}}},
    QueryResult{.rows = {{"0", "0"}}},
    QueryResult{.rows = {{"3", "-1"}}},
    QueryResult{.rows = {{"3", "0"}, {"3", "1"}}},
    QueryResult{.rows = {{"3", "0"}, {"5", "0"}}},
  };
  for (const auto& rows : malformed) {
    for (int operation = 0; operation < 3; ++operation) {
      MockPool mp(1);
      int rollback_count = 0;
      mp.connections[0]->query_hook = [rows](const std::string& sql,
                                             const std::vector<std::string>&) -> std::optional<QueryResult> {
        if (sql.find("music_meta") != std::string::npos)
          return QueryResult{.rows = {{"9"}}};
        if (sql.find("user_playlists") != std::string::npos)
          return QueryResult{.rows = {{"42"}}};
        return rows;
      };
      mp.connections[0]->execute_hook = [&rollback_count](const std::string& sql,
                                                          const std::vector<std::string>&) -> std::optional<int64_t> {
        if (sql == "ROLLBACK")
          ++rollback_count;
        return 1;
      };
      std::optional<MutationResult<std::monostate>> result;
      if (operation == 0) {
        result = mp.pool->add_playlist_item(7, 42, 9);
      } else if (operation == 1) {
        result = mp.pool->remove_playlist_item(7, 42, 3);
      } else {
        result = mp.pool->reorder_playlist_items(7, 42, {3, 5});
      }
      EXPECT_EQ(result->status, MutationStatus::INVALID_STATE) << operation;
      EXPECT_EQ(rollback_count, 1) << operation;
    }
  }
}

TEST(DatabasePoolPlaylistTest, RemoveRewritesEveryRemainingPositionAndRollsBackMidway) {
  const auto invoke = [](bool fail_second_rewrite) {
    MockPool mp(1);
    std::vector<std::vector<std::string>> rewrite_params;
    int rollback_count = 0;
    mp.connections[0]->query_hook = [](const std::string& sql,
                                       const std::vector<std::string>&) -> std::optional<QueryResult> {
      if (sql.find("user_playlists") != std::string::npos)
        return QueryResult{.rows = {{"42"}}};
      return QueryResult{.rows = {{"3", "2"}, {"5", "7"}, {"9", "11"}}};
    };
    mp.connections[0]->execute_hook = [&](const std::string& sql,
                                          const std::vector<std::string>& params) -> std::optional<int64_t> {
      if (sql == "ROLLBACK")
        ++rollback_count;
      if (sql == "UPDATE playlist_items SET sort_order = ? WHERE playlist_id = ? AND music_id = ?") {
        rewrite_params.push_back(params);
        if (fail_second_rewrite && rewrite_params.size() == 2)
          return std::nullopt;
      }
      return 1;
    };
    const auto result = mp.pool->remove_playlist_item(7, 42, 5);
    return std::tuple{result.status, rewrite_params, rollback_count};
  };

  const auto [status, params, rollback] = invoke(false);
  EXPECT_EQ(status, MutationStatus::OK);
  EXPECT_EQ(params, (std::vector<std::vector<std::string>>{{"0", "7", "3"}, {"1", "7", "9"}}));
  EXPECT_EQ(rollback, 0);

  const auto [failed_status, failed_params, failed_rollback] = invoke(true);
  EXPECT_EQ(failed_status, MutationStatus::STORAGE_ERROR);
  EXPECT_EQ(failed_params.size(), 2U);
  EXPECT_EQ(failed_rollback, 1);
}

TEST(DatabasePoolPlaylistTest, RemoveDistinguishesMissingMusicFromMissingPlaylist) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;
  mp.connections[0]->query_hook = [](const std::string& sql,
                                     const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    return QueryResult{.rows = {{"3", "0"}}};
  };

  const auto result = mp.pool->remove_playlist_item(7, 42, 99);

  EXPECT_EQ(result.status, MutationStatus::NOT_FOUND);
  EXPECT_EQ(result.detail, "MUSIC_NOT_FOUND");
}

TEST(DatabasePoolPlaylistTest, EveryTransactionalOperationRollsBackWhenCommitFails) {
  for (int operation = 0; operation < 7; ++operation) {
    MockPool mp(1);
    int rollback_count = 0;
    mp.connections[0]->last_insert_id_value = 7;
    mp.connections[0]->query_hook = [operation](const std::string& sql,
                                                const std::vector<std::string>&) -> std::optional<QueryResult> {
      if (sql.find("FROM users") != std::string::npos)
        return QueryResult{.rows = {{"42"}}};
      if (sql.find("SELECT user_id FROM user_playlists") != std::string::npos)
        return QueryResult{.rows = {{"42"}}};
      if (sql.find("SELECT pi.id") != std::string::npos)
        return QueryResult{};
      if (sql.find("SELECT p.playlist_id") != std::string::npos)
        return QueryResult{.rows = {{"7", "42", "name", "desc", "1", "now"}}};
      if (sql.find("music_meta") != std::string::npos)
        return QueryResult{.rows = {{"9"}}};
      if (sql.find("playlist_items") != std::string::npos)
        return operation == 4 ? QueryResult{} : QueryResult{.rows = {{"5", "0"}}};
      return QueryResult{};
    };
    mp.connections[0]->execute_hook = [&rollback_count](const std::string& sql,
                                                        const std::vector<std::string>&) -> std::optional<int64_t> {
      if (sql == "COMMIT")
        return std::nullopt;
      if (sql == "ROLLBACK")
        ++rollback_count;
      return 1;
    };
    MutationStatus status = MutationStatus::OK;
    Playlist playlist;
    playlist.user_id = 42;
    playlist.name = "name";
    switch (operation) {
      case 0:
        status = mp.pool->create_playlist(playlist, 42).status;
        break;
      case 1:
        status = mp.pool->update_playlist(7, 42, "name", "desc").status;
        break;
      case 2:
        status = mp.pool->delete_playlist(7, 42).status;
        break;
      case 3:
        status = mp.pool->get_playlist_items(7, 42).status;
        break;
      case 4:
        status = mp.pool->add_playlist_item(7, 42, 9).status;
        break;
      case 5:
        status = mp.pool->remove_playlist_item(7, 42, 5).status;
        break;
      case 6:
        status = mp.pool->reorder_playlist_items(7, 42, {5}).status;
        break;
      default:
        FAIL();
    }
    EXPECT_EQ(status, MutationStatus::STORAGE_ERROR) << operation;
    EXPECT_EQ(rollback_count, 1) << operation;
  }
}

TEST(DatabasePoolPlaylistTest, GetItemsRollsBackAtEverySnapshotFailureStage) {
  for (int failure_stage = 0; failure_stage < 4; ++failure_stage) {
    MockPool mp(1);
    int rollback_count = 0;
    int query_count = 0;
    mp.connections[0]->query_hook = [&query_count,
                                     failure_stage](const std::string& sql,
                                                    const std::vector<std::string>&) -> std::optional<QueryResult> {
      const int current = query_count++;
      if ((failure_stage == 1 && current == 0) || (failure_stage == 2 && current == 1))
        return std::nullopt;
      if (sql.find("user_playlists") != std::string::npos)
        return QueryResult{.rows = {{"42"}}};
      return QueryResult{};
    };
    mp.connections[0]->execute_hook = [failure_stage,
                                       &rollback_count](const std::string& sql,
                                                        const std::vector<std::string>&) -> std::optional<int64_t> {
      if (sql == "ROLLBACK") {
        ++rollback_count;
        return 1;
      }
      if ((failure_stage == 0 && sql == "START TRANSACTION WITH CONSISTENT SNAPSHOT") ||
          (failure_stage == 3 && sql == "COMMIT")) {
        return std::nullopt;
      }
      return 1;
    };

    const auto result = mp.pool->get_playlist_items(7, 42);

    EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR) << failure_stage;
    EXPECT_FALSE(result.value.has_value()) << failure_stage;
    EXPECT_EQ(rollback_count, 1) << failure_stage;
  }
}

TEST(DatabasePoolPlaylistTest, ValidatesUtf8NameAndDescriptionByCodePointBeforeWriting) {
  const auto repeat = [](std::string_view value, std::size_t count) {
    std::string result;
    for (std::size_t index = 0; index < count; ++index) result += value;
    return result;
  };
  const std::string chinese = "歌";
  MockPool mp(1);
  mp.connections[0]->last_insert_id_value = 7;
  mp.connections[0]->execute_result = 1;
  mp.connections[0]->query_hook = [](const std::string& sql,
                                     const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("FROM users") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    return QueryResult{.rows = {{"7", "42", "name", "desc", "0", "now"}}};
  };
  Playlist playlist;
  playlist.user_id = 42;
  playlist.name = repeat(chinese, 128);
  playlist.description = repeat(chinese, 512);
  EXPECT_EQ(mp.pool->create_playlist(playlist, 42).status, MutationStatus::OK);

  playlist.name = repeat(chinese, 129);
  EXPECT_EQ(mp.pool->create_playlist(playlist, 42).status, MutationStatus::INVALID_STATE);
  playlist.name = "valid";
  playlist.description = repeat(chinese, 513);
  EXPECT_EQ(mp.pool->create_playlist(playlist, 42).status, MutationStatus::INVALID_STATE);
  playlist.description.clear();
  playlist.name = std::string(1, static_cast<char>(0xFF));
  EXPECT_EQ(mp.pool->create_playlist(playlist, 42).status, MutationStatus::INVALID_STATE);

  for (const std::string& invalid : {std::string{"\x80", 1},
                                     std::string{"\xC0\xAF", 2},
                                     std::string{"\xED\xA0\x80", 3},
                                     std::string{"\xF4\x90\x80\x80", 4}}) {
    playlist.name = invalid;
    EXPECT_EQ(mp.pool->create_playlist(playlist, 42).status, MutationStatus::INVALID_STATE);
    playlist.name = "valid";
    playlist.description = invalid;
    EXPECT_EQ(mp.pool->create_playlist(playlist, 42).status, MutationStatus::INVALID_STATE);
  }
}

TEST(DatabasePoolVipTest, GrantUsesMaxOfNowAndCurrentExpiryForEveryAllowedDuration) {
  constexpr auto now = std::chrono::system_clock::time_point{std::chrono::seconds{2'000'000'000}};
  for (const int days : {30, 90, 365}) {
    MockPool mp(1);
    auto current_expiry = now + std::chrono::hours{24 * 10};
    std::string written_expiry;
    mp.connections[0]->query_hook = [current_expiry](const std::string& sql,
                                                     const std::vector<std::string>&) -> std::optional<QueryResult> {
      EXPECT_NE(sql.find("FOR UPDATE"), std::string::npos);
      return QueryResult{.rows = {{"7",
                                   "vip-user",
                                   "hash",
                                   "salt",
                                   "2",
                                   "vip@example.com",
                                   format_mysql_utc_datetime(current_expiry),
                                   "2026-01-02 03:04:05.000000"}}};
    };
    mp.connections[0]->execute_hook =
      [&written_expiry](const std::string& sql, const std::vector<std::string>& params) -> std::optional<int64_t> {
      if (sql.starts_with("UPDATE users SET role = 2")) {
        written_expiry = params.at(0);
        return 1;
      }
      return 0;
    };

    const auto result = mp.pool->grant_or_extend_vip(7, days, now);

    ASSERT_EQ(result.status, MutationStatus::OK);
    ASSERT_TRUE(result.value.has_value());
    const auto expected = current_expiry + std::chrono::hours{24 * days};
    EXPECT_EQ(result.value->role, UserRole::VIP);
    EXPECT_EQ(result.value->vip_expires_at, expected);
    EXPECT_EQ(written_expiry, format_mysql_utc_datetime(expected));
  }
}

TEST(DatabasePoolVipTest, GrantStartsExpiredMembershipFromNow) {
  constexpr auto now = std::chrono::system_clock::time_point{std::chrono::seconds{2'000'000'000}};
  MockPool mp(1);
  const auto expired = now - std::chrono::seconds{1};
  mp.connections[0]->query_result = QueryResult{
    .rows = {
      {"7", "expired", "hash", "salt", "2", "", format_mysql_utc_datetime(expired), "2026-01-02 03:04:05.000000"}}};
  mp.connections[0]->execute_result = 1;

  const auto result = mp.pool->grant_or_extend_vip(7, 30, now);

  ASSERT_EQ(result.status, MutationStatus::OK);
  ASSERT_TRUE(result.value.has_value());
  EXPECT_EQ(result.value->vip_expires_at, now + std::chrono::hours{24 * 30});
}

TEST(DatabasePoolVipTest, MutationDistinguishesNotFoundAdminInvalidDataAndStorage) {
  constexpr auto now = std::chrono::system_clock::time_point{std::chrono::seconds{2'000'000'000}};
  MockPool mp(1);
  mp.connections[0]->execute_result = 0;

  mp.connections[0]->query_result = QueryResult{};
  EXPECT_EQ(mp.pool->grant_or_extend_vip(7, 30, now).status, MutationStatus::NOT_FOUND);

  mp.connections[0]->query_result =
    QueryResult{.rows = {{"1", "admin", "hash", "salt", "3", "admin@example.com", "", "2026-01-02 03:04:05.000000"}}};
  EXPECT_EQ(mp.pool->grant_or_extend_vip(1, 30, now).status, MutationStatus::CONFLICT);
  EXPECT_EQ(mp.pool->revoke_vip(1).status, MutationStatus::CONFLICT);

  mp.connections[0]->query_result = QueryResult{.rows = {{"broken", "row"}}};
  EXPECT_EQ(mp.pool->grant_or_extend_vip(7, 30, now).status, MutationStatus::INVALID_STATE);

  mp.connections[0]->query_result =
    QueryResult{.rows = {{"7", "guest", "hash", "salt", "0", "", "", "2026-01-02 03:04:05.000000"}}};
  EXPECT_EQ(mp.pool->grant_or_extend_vip(7, 30, now).status, MutationStatus::INVALID_STATE);

  mp.connections[0]->query_result = std::nullopt;
  EXPECT_EQ(mp.pool->grant_or_extend_vip(7, 30, now).status, MutationStatus::STORAGE_ERROR);
}

TEST(DatabasePoolVipTest, GrantAndRevokeRejectEveryRoleExpiryInvariantViolation) {
  constexpr auto now = std::chrono::system_clock::time_point{std::chrono::seconds{2'000'000'000}};
  const std::array inconsistent_states = {
    std::pair{UserRole::NORMAL, std::string_view{"2034-01-01 00:00:00.000000"}},
    std::pair{UserRole::VIP, std::string_view{}},
    std::pair{UserRole::ADMIN, std::string_view{"2034-01-01 00:00:00.000000"}},
    std::pair{UserRole::GUEST, std::string_view{"2034-01-01 00:00:00.000000"}},
  };

  for (const auto& [role, expires_at] : inconsistent_states) {
    for (const bool grant : {true, false}) {
      MockPool mp(1);
      int rollback_count = 0;
      int update_count = 0;
      mp.connections[0]->query_result = QueryResult{.rows = {{"7",
                                                              "inconsistent",
                                                              "hash",
                                                              "salt",
                                                              std::to_string(static_cast<int>(role)),
                                                              "",
                                                              std::string(expires_at),
                                                              "2026-01-02 03:04:05.000000"}}};
      mp.connections[0]->execute_hook = [&rollback_count,
                                         &update_count](const std::string& sql,
                                                        const std::vector<std::string>&) -> std::optional<int64_t> {
        if (sql == "ROLLBACK") {
          ++rollback_count;
          return 0;
        }
        if (sql.starts_with("UPDATE users")) {
          ++update_count;
        }
        return 0;
      };

      const auto result = grant ? mp.pool->grant_or_extend_vip(7, 30, now) : mp.pool->revoke_vip(7);

      EXPECT_EQ(result.status, MutationStatus::INVALID_STATE) << static_cast<int>(role) << grant;
      EXPECT_EQ(result.detail, "VIP_STATE_INVALID") << static_cast<int>(role) << grant;
      EXPECT_FALSE(result.value.has_value()) << static_cast<int>(role) << grant;
      EXPECT_EQ(update_count, 0) << static_cast<int>(role) << grant;
      EXPECT_EQ(rollback_count, 1) << static_cast<int>(role) << grant;
    }
  }
}

TEST(DatabasePoolVipTest, EveryTransactionFailureRollsBackAndReturnsNoValue) {
  constexpr auto now = std::chrono::system_clock::time_point{std::chrono::seconds{2'000'000'000}};
  for (int failure_stage = 0; failure_stage < 4; ++failure_stage) {
    MockPool mp(1);
    int rollback_count = 0;
    bool pending_vip = false;
    UserRole persisted_role = UserRole::NORMAL;
    mp.connections[0]->query_hook = [failure_stage](const std::string&,
                                                    const std::vector<std::string>&) -> std::optional<QueryResult> {
      if (failure_stage == 1) {
        return std::nullopt;
      }
      return QueryResult{
        .rows = {{"7", "normal", "hash", "salt", "1", "normal@example.com", "", "2026-01-02 03:04:05.000000"}}};
    };
    mp.connections[0]->execute_hook = [failure_stage,
                                       &rollback_count,
                                       &pending_vip,
                                       &persisted_role](const std::string& sql,
                                                        const std::vector<std::string>&) -> std::optional<int64_t> {
      if (sql == "ROLLBACK") {
        ++rollback_count;
        pending_vip = false;
        return 0;
      }
      if ((failure_stage == 0 && sql == "START TRANSACTION") ||
          (failure_stage == 2 && sql.starts_with("UPDATE users SET role = 2")) ||
          (failure_stage == 3 && sql == "COMMIT")) {
        return std::nullopt;
      }
      if (sql.starts_with("UPDATE")) {
        pending_vip = true;
        return 1;
      }
      if (sql == "COMMIT" && pending_vip) {
        persisted_role = UserRole::VIP;
      }
      return 0;
    };

    const auto result = mp.pool->grant_or_extend_vip(7, 30, now);

    EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR) << failure_stage;
    EXPECT_FALSE(result.value.has_value()) << failure_stage;
    EXPECT_EQ(rollback_count, 1) << failure_stage;
    EXPECT_EQ(persisted_role, UserRole::NORMAL) << failure_stage;
  }
}

TEST(DatabasePoolVipTest, ConnectionAcquisitionFailureNeverReturnsOkOrAValue) {
  constexpr auto now = std::chrono::system_clock::time_point{std::chrono::seconds{2'000'000'000}};
  MockPool mp(1);
  mp.pool->close();

  const auto grant = mp.pool->grant_or_extend_vip(7, 30, now);
  const auto revoke = mp.pool->revoke_vip(7);

  EXPECT_EQ(grant.status, MutationStatus::STORAGE_ERROR);
  EXPECT_FALSE(grant.value.has_value());
  EXPECT_EQ(revoke.status, MutationStatus::STORAGE_ERROR);
  EXPECT_FALSE(revoke.value.has_value());
}

TEST(DatabasePoolVipTest, GrantRejectsExpiryThatWouldOverflowSystemClock) {
  const auto now = std::chrono::system_clock::time_point::max() - std::chrono::hours{24};
  MockPool mp(1);
  int update_count = 0;
  mp.connections[0]->query_result =
    QueryResult{.rows = {{"7", "normal", "hash", "salt", "1", "", "", "2026-01-02 03:04:05.000000"}}};
  mp.connections[0]->execute_hook = [&update_count](const std::string& sql,
                                                    const std::vector<std::string>&) -> std::optional<int64_t> {
    if (sql.starts_with("UPDATE users")) {
      ++update_count;
    }
    return 0;
  };

  const auto result = mp.pool->grant_or_extend_vip(7, 30, now);

  EXPECT_EQ(result.status, MutationStatus::INVALID_STATE);
  EXPECT_FALSE(result.value.has_value());
  EXPECT_EQ(update_count, 0);
}

TEST(DatabasePoolVipTest, RevokeRejectsGuestWithoutUpdating) {
  MockPool mp(1);
  int update_count = 0;
  mp.connections[0]->query_result =
    QueryResult{.rows = {{"7", "guest", "hash", "salt", "0", "", "", "2026-01-02 03:04:05.000000"}}};
  mp.connections[0]->execute_hook = [&update_count](const std::string& sql,
                                                    const std::vector<std::string>&) -> std::optional<int64_t> {
    if (sql.starts_with("UPDATE users")) {
      ++update_count;
      return 1;
    }
    return 0;
  };

  const auto result = mp.pool->revoke_vip(7);

  EXPECT_EQ(result.status, MutationStatus::INVALID_STATE);
  EXPECT_EQ(result.detail, "VIP_STATE_INVALID");
  EXPECT_FALSE(result.value.has_value());
  EXPECT_EQ(update_count, 0);
}

TEST(DatabasePoolVipTest, RevokeFailureAtEveryTransactionStageRollsBackWithoutReturningValue) {
  for (int failure_stage = 0; failure_stage < 4; ++failure_stage) {
    MockPool mp(1);
    int rollback_count = 0;
    bool pending_revoke = false;
    UserRole persisted_role = UserRole::VIP;
    mp.connections[0]->query_hook = [failure_stage](const std::string&,
                                                    const std::vector<std::string>&) -> std::optional<QueryResult> {
      if (failure_stage == 1) {
        return std::nullopt;
      }
      return QueryResult{.rows = {{"7",
                                   "vip",
                                   "hash",
                                   "salt",
                                   "2",
                                   "vip@example.com",
                                   "2034-01-01 00:00:00.000000",
                                   "2026-01-02 03:04:05.000000"}}};
    };
    mp.connections[0]->execute_hook = [failure_stage,
                                       &rollback_count,
                                       &pending_revoke,
                                       &persisted_role](const std::string& sql,
                                                        const std::vector<std::string>&) -> std::optional<int64_t> {
      if (sql == "ROLLBACK") {
        ++rollback_count;
        pending_revoke = false;
        return 0;
      }
      if ((failure_stage == 0 && sql == "START TRANSACTION") ||
          (failure_stage == 2 && sql.starts_with("UPDATE users SET role = 1")) ||
          (failure_stage == 3 && sql == "COMMIT")) {
        return std::nullopt;
      }
      if (sql.starts_with("UPDATE users SET role = 1")) {
        pending_revoke = true;
        return 1;
      }
      if (sql == "COMMIT" && pending_revoke) {
        persisted_role = UserRole::NORMAL;
      }
      return 0;
    };

    const auto result = mp.pool->revoke_vip(7);

    EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR) << failure_stage;
    EXPECT_FALSE(result.value.has_value()) << failure_stage;
    EXPECT_EQ(rollback_count, 1) << failure_stage;
    EXPECT_EQ(persisted_role, UserRole::VIP) << failure_stage;
  }
}

TEST(DatabasePoolVipTest, RevokeClosesAfterRollbackFailureAndReconnectsBeforeReuse) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  connection->query_result = QueryResult{.rows = {{"7",
                                                   "vip",
                                                   "hash",
                                                   "salt",
                                                   "2",
                                                   "vip@example.com",
                                                   "2034-01-01 00:00:00.000000",
                                                   "2026-01-02 03:04:05.000000"}}};
  connection->execute_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<int64_t> {
    if (sql == "ROLLBACK" || sql.starts_with("UPDATE users SET role = 1")) {
      return std::nullopt;
    }
    return 0;
  };
  connection->close_hook = [connection]() { connection->is_open_result = false; };
  connection->connect_hook = [connection]() { connection->is_open_result = true; };

  const auto result = mp.pool->revoke_vip(7);

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  EXPECT_FALSE(result.value.has_value());
  EXPECT_EQ(connection->close_count, 1);
  EXPECT_EQ(connection->connect_count, 1);
  EXPECT_TRUE(mp.pool->with_connection([](IConnection&) { return true; }));
  EXPECT_EQ(connection->connect_count, 2);
}

TEST(DatabasePoolVipTest, RevokePersistsNormalRoleAndNullExpiryInOneTransaction) {
  MockPool mp(1);
  std::vector<std::string> statements;
  mp.connections[0]->query_result = QueryResult{.rows = {{"7",
                                                          "vip",
                                                          "hash",
                                                          "salt",
                                                          "2",
                                                          "vip@example.com",
                                                          "2034-01-01 00:00:00.000000",
                                                          "2026-01-02 03:04:05.000000"}}};
  mp.connections[0]->execute_hook = [&statements](const std::string& sql,
                                                  const std::vector<std::string>&) -> std::optional<int64_t> {
    statements.push_back(sql);
    return sql.starts_with("UPDATE") ? 1 : 0;
  };

  const auto result = mp.pool->revoke_vip(7);

  ASSERT_EQ(result.status, MutationStatus::OK);
  ASSERT_TRUE(result.value.has_value());
  EXPECT_EQ(result.value->role, UserRole::NORMAL);
  EXPECT_FALSE(result.value->vip_expires_at.has_value());
  ASSERT_EQ(statements.size(), 3U);
  EXPECT_EQ(statements[0], "START TRANSACTION");
  EXPECT_NE(statements[1].find("vip_expires_at = NULL"), std::string::npos);
  EXPECT_EQ(statements[2], "COMMIT");
}

TEST(DatabasePoolAdminTest, ListUsersUsesSameDecodedFilterAndStableAscendingPagination) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 0;
  std::vector<std::string> sqls;
  std::vector<std::vector<std::string>> params;
  mp.connections[0]->query_hook =
    [&sqls, &params](const std::string& sql,
                     const std::vector<std::string>& query_params) -> std::optional<QueryResult> {
    sqls.push_back(sql);
    params.push_back(query_params);
    if (sql.starts_with("SELECT COUNT")) {
      return QueryResult{.rows = {{"2"}}};
    }
    return QueryResult{
      .rows = {{"2", "alice", "hash", "salt", "1", "alice@example.com", "", "2026-01-02 03:04:05.000000"},
               {"9",
                "bob",
                "hash",
                "salt",
                "2",
                "bob@example.com",
                "2034-01-01 00:00:00.000000",
                "2026-02-03 04:05:06.000000"}}};
  };

  const auto result = mp.pool->list_admin_users("example.com", 3, 5);

  ASSERT_EQ(result.status, LookupStatus::FOUND);
  ASSERT_TRUE(result.value.has_value());
  EXPECT_EQ(result.value->total, 2);
  ASSERT_EQ(result.value->items.size(), 2U);
  ASSERT_EQ(sqls.size(), 2U);
  EXPECT_NE(sqls[0].find("username LIKE ? ESCAPE '\\\\' OR email LIKE ? ESCAPE '\\\\'"), std::string::npos);
  EXPECT_NE(sqls[1].find("ORDER BY user_id ASC LIMIT ? OFFSET ?"), std::string::npos);
  EXPECT_EQ(params[0], (std::vector<std::string>{"%example.com%", "%example.com%"}));
  EXPECT_EQ(params[1], (std::vector<std::string>{"%example.com%", "%example.com%", "5", "3"}));
}

TEST(DatabasePoolAdminTest, ListUsersKeepsCountAndItemsInOneConsistentSnapshot) {
  MockPool mp(1);
  int live_version = 1;
  int snapshot_version = 0;
  bool transaction_active = false;
  std::vector<std::string> statements;
  mp.connections[0]->execute_hook = [&live_version,
                                     &snapshot_version,
                                     &transaction_active,
                                     &statements](const std::string& sql,
                                                  const std::vector<std::string>&) -> std::optional<int64_t> {
    statements.push_back(sql);
    if (sql == "START TRANSACTION WITH CONSISTENT SNAPSHOT") {
      transaction_active = true;
      snapshot_version = live_version;
    }
    if (sql == "COMMIT" || sql == "ROLLBACK") {
      transaction_active = false;
    }
    return 0;
  };
  mp.connections[0]->query_hook = [&live_version,
                                   &snapshot_version,
                                   &transaction_active](const std::string& sql,
                                                        const std::vector<std::string>&) -> std::optional<QueryResult> {
    const int visible_version = transaction_active ? snapshot_version : live_version;
    if (sql.starts_with("SELECT COUNT")) {
      auto result = QueryResult{.rows = {{std::to_string(visible_version)}}};
      live_version = 2;
      return result;
    }
    QueryResult result;
    result.rows.push_back({"1", "first", "hash", "salt", "1", "", "", "2026-01-02 03:04:05.000000"});
    if (visible_version == 2) {
      result.rows.push_back({"2", "concurrent", "hash", "salt", "1", "", "", "2026-01-02 03:04:05.000000"});
    }
    return result;
  };

  const auto result = mp.pool->list_admin_users("", 0, 20);

  ASSERT_EQ(result.status, LookupStatus::FOUND);
  ASSERT_TRUE(result.value.has_value());
  EXPECT_EQ(result.value->total, 1);
  EXPECT_EQ(result.value->items.size(), 1U);
  EXPECT_EQ(statements, (std::vector<std::string>{"START TRANSACTION WITH CONSISTENT SNAPSHOT", "COMMIT"}));
}

TEST(DatabasePoolAdminTest, ListUsersFailureAtEverySnapshotStageRollsBackWithoutReturningValue) {
  for (int failure_stage = 0; failure_stage < 4; ++failure_stage) {
    MockPool mp(1);
    int rollback_count = 0;
    mp.connections[0]->execute_hook = [failure_stage,
                                       &rollback_count](const std::string& sql,
                                                        const std::vector<std::string>&) -> std::optional<int64_t> {
      if (sql == "ROLLBACK") {
        ++rollback_count;
        return 0;
      }
      if ((failure_stage == 0 && sql == "START TRANSACTION WITH CONSISTENT SNAPSHOT") ||
          (failure_stage == 3 && sql == "COMMIT")) {
        return std::nullopt;
      }
      return 0;
    };
    mp.connections[0]->query_hook = [failure_stage](const std::string& sql,
                                                    const std::vector<std::string>&) -> std::optional<QueryResult> {
      if ((failure_stage == 1 && sql.starts_with("SELECT COUNT")) ||
          (failure_stage == 2 && !sql.starts_with("SELECT COUNT"))) {
        return std::nullopt;
      }
      return sql.starts_with("SELECT COUNT")
               ? QueryResult{.rows = {{"1"}}}
               : QueryResult{.rows = {{"1", "first", "hash", "salt", "1", "", "", "2026-01-02 03:04:05.000000"}}};
    };

    const auto result = mp.pool->list_admin_users("", 0, 20);

    EXPECT_EQ(result.status, LookupStatus::STORAGE_ERROR) << failure_stage;
    EXPECT_FALSE(result.value.has_value()) << failure_stage;
    EXPECT_EQ(rollback_count, 1) << failure_stage;
  }
}

TEST(DatabasePoolAdminTest, ListUsersClosesAfterRollbackFailureAndReconnectsBeforeReuse) {
  MockPool mp(1);
  auto* connection = mp.connections[0];
  connection->execute_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<int64_t> {
    if (sql == "ROLLBACK" || sql == "COMMIT") {
      return std::nullopt;
    }
    return 0;
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<QueryResult> {
    return sql.starts_with("SELECT COUNT") ? QueryResult{.rows = {{"0"}}} : QueryResult{};
  };
  connection->close_hook = [connection]() { connection->is_open_result = false; };
  connection->connect_hook = [connection]() { connection->is_open_result = true; };

  const auto result = mp.pool->list_admin_users("", 0, 20);

  EXPECT_EQ(result.status, LookupStatus::STORAGE_ERROR);
  EXPECT_FALSE(result.value.has_value());
  EXPECT_EQ(connection->close_count, 1);
  EXPECT_EQ(connection->connect_count, 1);
  EXPECT_TRUE(mp.pool->with_connection([](IConnection&) { return true; }));
  EXPECT_EQ(connection->connect_count, 2);
}

TEST(DatabasePoolAdminTest, ListUsersTreatsLikeWildcardsAsLiteralSearchText) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 0;
  std::vector<std::string> sqls;
  std::vector<std::vector<std::string>> params;
  mp.connections[0]->query_hook =
    [&sqls, &params](const std::string& sql,
                     const std::vector<std::string>& query_params) -> std::optional<QueryResult> {
    sqls.push_back(sql);
    params.push_back(query_params);
    return sql.starts_with("SELECT COUNT") ? QueryResult{.rows = {{"0"}}} : QueryResult{};
  };

  const auto result = mp.pool->list_admin_users(R"(50%_off\today)", 0, 20);

  ASSERT_EQ(result.status, LookupStatus::FOUND);
  ASSERT_EQ(sqls.size(), 2U);
  EXPECT_NE(sqls[0].find("LIKE ? ESCAPE '\\\\'"), std::string::npos);
  EXPECT_NE(sqls[1].find("LIKE ? ESCAPE '\\\\'"), std::string::npos);
  const std::string expected = R"(%50\%\_off\\today%)";
  EXPECT_EQ(params[0], (std::vector<std::string>{expected, expected}));
  EXPECT_EQ(params[1], (std::vector<std::string>{expected, expected, "20", "0"}));
}

// T-PLAYLIST-DEDUP: 一个 music_id 关联两条音频文件时不产生重复 PlaylistItem
TEST(DatabasePoolPlaylistTest, GetPlaylistItemsDeduplicatesMultipleAudioFiles) {
  MockPool mp(1);
  mp.connections[0]->execute_result = 1;

  mp.connections[0]->query_hook = [](const std::string& sql,
                                     const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"1"}}};
    // 断言 SQL 不含 LEFT JOIN file_records（已改为标量子查询）
    EXPECT_EQ(sql.find("LEFT JOIN file_records"), std::string::npos) << "SQL should use scalar subquery, not LEFT JOIN";
    // 返回 1 行（标量子查询正确时）
    QueryResult qr;
    qr.rows.push_back({"1", "1", "5", "0", "2024-01-01 00:00:00", "Song", "Artist", "hash-mp3"});
    return qr;
  };

  auto items = mp.pool->get_playlist_items(1, 1);
  ASSERT_EQ(items.status, MutationStatus::OK);
  ASSERT_TRUE(items.value.has_value());
  // 关键断言：只有 1 项，不是 2 项
  ASSERT_EQ(items.value->size(), 1U) << "Should return exactly 1 PlaylistItem even if music has 2 audio files";
  EXPECT_EQ(items.value->at(0).file_hash, "hash-mp3");
}

// T-PLAYLIST-ORDERBY: get_user_playlists 使用带 tie-break 的稳定排序
TEST(DatabasePoolPlaylistTest, GetUserPlaylistsUsesStableOrderBy) {
  MockPool mp(1);
  std::string captured_sql;
  mp.connections[0]->query_hook = [&captured_sql](const std::string& sql,
                                                  const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("FROM user_playlists") != std::string::npos) {
      captured_sql = sql;
      QueryResult qr;
      qr.rows.push_back({"1", "1", "A", "", "0", "2024-01-01 00:00:00"});
      qr.rows.push_back({"2", "1", "B", "", "0", "2024-01-01 00:00:00"});
      return qr;
    }
    return std::nullopt;
  };
  auto result = mp.pool->get_user_playlists(1, 1);
  ASSERT_EQ(result.status, MutationStatus::OK);
  // 断言 ORDER BY 子句含确定性二级排序键 playlist_id DESC
  EXPECT_NE(captured_sql.find("playlist_id DESC"), std::string::npos)
    << "ORDER BY must include playlist_id DESC as tie-break";
}

} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
