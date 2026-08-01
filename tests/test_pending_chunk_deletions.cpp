#include "database_pool.h"
#include "file_system.h"
#include "mock_connection.h"
#include "pending_chunk_deletions.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace hps {
namespace {

template <typename Guard>
Guard move_guard(Guard& guard) {
  return std::move(guard);
}

class StubPendingDatabase final : public IDatabasePool {
public:
  MutationResult<std::vector<PendingChunkDeletion>> claim_result;
  std::vector<MutationResult<std::vector<PendingChunkDeletion>>> claim_results;
  std::vector<std::pair<std::string, std::string>> completed;
  std::vector<std::pair<std::string, std::string>> released;
  std::vector<std::string> released_errors;
  std::vector<std::string> reference_checks;
  LookupResult<bool> reference_result{LookupStatus::FOUND, false};
  std::vector<LookupResult<bool>> reference_results;
  MutationResult<std::monostate> complete_result{MutationStatus::OK, std::monostate{}, std::nullopt};
  std::vector<MutationResult<std::monostate>> complete_results;
  MutationResult<std::monostate> release_result{MutationStatus::OK, std::monostate{}, std::nullopt};
  std::vector<MutationResult<std::monostate>> release_results;
  MutationResult<std::monostate> cancel_result{MutationStatus::OK, std::monostate{}, std::nullopt};
  std::vector<MutationResult<std::monostate>> cancel_results;
  std::vector<std::pair<std::string, std::string>> cancelled;
  std::size_t observed_limit{0};
  bool throw_on_claim{false};
  bool throw_on_complete{false};
  bool throw_on_release{false};
  bool throw_on_reference{false};
  bool throw_on_cancel{false};

  bool init(const DbConfig& config) override {
    (void)config;
    return true;
  }

  void close() override {}

  LookupResult<User> get_user_result(int64_t user_id) override {
    (void)user_id;
    return {};
  }

  MutationResult<std::monostate> create_user(const User& user) override {
    (void)user;
    return {};
  }

  MutationResult<std::monostate> update_user(const User& user) override {
    (void)user;
    return {};
  }

  bool username_exists(const std::string& username) override {
    (void)username;
    return false;
  }

  std::optional<int64_t> store_file_record(const FileRecord& record) override {
    (void)record;
    return std::nullopt;
  }

  std::optional<FileRecord> get_file_record(int64_t file_id) override {
    (void)file_id;
    return std::nullopt;
  }

  LookupResult<FileRecord> get_file_record_result(int64_t file_id) override {
    (void)file_id;
    return {};
  }

  std::optional<FileRecord> get_file_record_by_hash(const std::string& hash) override {
    (void)hash;
    return std::nullopt;
  }

  std::vector<FileRecord> search_files(const std::string& name_pattern, int offset, int limit) override {
    (void)name_pattern;
    (void)offset;
    (void)limit;
    return {};
  }

  std::vector<FileRecord> search_files_ext(const std::string& name_pattern,
                                           const std::string& type_filter,
                                           int offset,
                                           int limit,
                                           int& out_total) override {
    (void)name_pattern;
    (void)type_filter;
    (void)offset;
    (void)limit;
    (void)out_total;
    return {};
  }

  bool update_file_record(const FileRecord& record) override {
    (void)record;
    return false;
  }

  bool store_file_chunks(const std::vector<FileChunkRecord>& chunks) override {
    (void)chunks;
    return false;
  }

  std::vector<FileChunkRecord> get_file_chunks(const std::string& file_hash) override {
    (void)file_hash;
    return {};
  }

  bool chunk_exists(const std::string& chunk_hash) override {
    (void)chunk_hash;
    return false;
  }

  LookupResult<AuthUser> get_auth_user_result(const std::string& username) override {
    (void)username;
    return {};
  }

  bool verify_password(const std::string& username, const std::string& password) override {
    (void)username;
    (void)password;
    return false;
  }

  std::vector<MusicMeta> list_music_library(const std::string& search, int offset, int limit, int& out_total) override {
    (void)search;
    (void)offset;
    (void)limit;
    (void)out_total;
    return {};
  }

  std::optional<MusicMeta> get_music_meta(int64_t music_id) override {
    (void)music_id;
    return std::nullopt;
  }

  std::optional<MusicMeta> get_music_by_file_id(int64_t file_id) override {
    (void)file_id;
    return std::nullopt;
  }

  int64_t create_music_meta(const MusicMeta& meta) override {
    (void)meta;
    return 0;
  }

  bool update_music_meta(const MusicMeta& meta) override {
    (void)meta;
    return false;
  }

  bool delete_music_meta(int64_t music_id) override {
    (void)music_id;
    return false;
  }

  MutationResult<std::vector<Playlist>> get_user_playlists(int64_t user_id, int64_t actor_id) override {
    (void)user_id;
    (void)actor_id;
    return {};
  }

  MutationResult<Playlist> create_playlist(const Playlist& playlist, int64_t actor_id) override {
    (void)playlist;
    (void)actor_id;
    return {};
  }

  MutationResult<Playlist> update_playlist(int64_t playlist_id,
                                           int64_t actor_id,
                                           const std::string& name,
                                           const std::string& description) override {
    (void)playlist_id;
    (void)actor_id;
    (void)name;
    (void)description;
    return {};
  }

  MutationResult<std::monostate> delete_playlist(int64_t playlist_id, int64_t actor_id) override {
    (void)playlist_id;
    (void)actor_id;
    return {};
  }

  MutationResult<std::vector<PlaylistItem>> get_playlist_items(int64_t playlist_id, int64_t actor_id) override {
    (void)playlist_id;
    (void)actor_id;
    return {};
  }

  MutationResult<std::monostate> add_playlist_item(int64_t playlist_id, int64_t actor_id, int64_t music_id) override {
    (void)playlist_id;
    (void)actor_id;
    (void)music_id;
    return {};
  }

  MutationResult<std::monostate> remove_playlist_item(int64_t playlist_id,
                                                      int64_t actor_id,
                                                      int64_t music_id) override {
    (void)playlist_id;
    (void)actor_id;
    (void)music_id;
    return {};
  }

  MutationResult<std::monostate> reorder_playlist_items(int64_t playlist_id,
                                                        int64_t actor_id,
                                                        const std::vector<int64_t>& music_ids) override {
    (void)playlist_id;
    (void)actor_id;
    (void)music_ids;
    return {};
  }

  MutationResult<std::vector<PendingChunkDeletion>> claim_pending_chunk_deletions(
    std::size_t limit,
    std::chrono::system_clock::time_point stale_before) override {
    (void)stale_before;
    observed_limit = limit;
    if (throw_on_claim) {
      throw std::runtime_error("claim failed");
    }
    if (!claim_results.empty()) {
      auto result = std::move(claim_results.front());
      claim_results.erase(claim_results.begin());
      return result;
    }
    return claim_result;
  }

  MutationResult<std::monostate> complete_pending_chunk_deletion(const std::string& hash,
                                                                 const std::string& token) override {
    completed.emplace_back(hash, token);
    if (throw_on_complete) {
      throw std::runtime_error("complete failed");
    }
    if (!complete_results.empty()) {
      auto result = std::move(complete_results.front());
      complete_results.erase(complete_results.begin());
      return result;
    }
    return complete_result;
  }

  MutationResult<std::monostate> release_pending_chunk_deletion(const std::string& hash,
                                                                const std::string& token,
                                                                const std::string& error) override {
    released.emplace_back(hash, token);
    released_errors.push_back(error);
    if (throw_on_release) {
      throw std::runtime_error("release failed");
    }
    if (!release_results.empty()) {
      auto result = std::move(release_results.front());
      release_results.erase(release_results.begin());
      return result;
    }
    return release_result;
  }

  LookupResult<bool> has_chunk_references(const std::string& hash) override {
    reference_checks.push_back(hash);
    if (throw_on_reference) {
      throw std::runtime_error("reference lookup failed");
    }
    if (!reference_results.empty()) {
      auto result = reference_results.front();
      reference_results.erase(reference_results.begin());
      return result;
    }
    return reference_result;
  }

  MutationResult<std::monostate> cancel_pending_chunk_deletion(const std::string& hash,
                                                               const std::string& token) override {
    cancelled.emplace_back(hash, token);
    if (throw_on_cancel) {
      throw std::runtime_error("cancel failed");
    }
    if (!cancel_results.empty()) {
      auto result = std::move(cancel_results.front());
      cancel_results.erase(cancel_results.begin());
      return result;
    }
    return cancel_result;
  }
};

class StatusFileSystem final : public IFileSystem {
public:
  ChunkDeleteStatus status{ChunkDeleteStatus::DELETED};
  std::vector<ChunkDeleteStatus> statuses;
  bool throw_on_delete{false};
  std::vector<std::string> paths;
  mutable std::mutex paths_mutex;

  std::vector<FileChunk> split_file(const std::string& path, std::size_t chunk_size) override {
    (void)path;
    (void)chunk_size;
    return {};
  }

  std::string compute_file_hash(const std::string& path) override {
    (void)path;
    return {};
  }

  std::string compute_chunk_hash(const FileChunk& chunk) override {
    (void)chunk;
    return {};
  }

  bool store_file(const std::string& path, const std::vector<char>& data) override {
    (void)path;
    (void)data;
    return false;
  }

  bool delete_file(const std::string& path) override {
    (void)path;
    return false;
  }

  std::optional<std::vector<char>> read_file(const std::string& path) override {
    (void)path;
    return std::nullopt;
  }

  ChunkDeleteStatus delete_file_status(const std::string& path) override {
    {
      std::lock_guard lock(paths_mutex);
      paths.push_back(path);
    }
    if (throw_on_delete) {
      throw std::runtime_error("absolute /secret/path must not leak");
    }
    if (!statuses.empty()) {
      const auto current = statuses.front();
      statuses.erase(statuses.begin());
      return current;
    }
    return status;
  }

  bool paths_empty() const {
    std::lock_guard lock(paths_mutex);
    return paths.empty();
  }
};

class ObservedDatabasePool final : public DatabasePool {
public:
  using DatabasePool::DatabasePool;

  bool with_connection(const std::function<bool(IConnection&)>& operation) override {
    ++with_connection_calls;
    return DatabasePool::with_connection(operation);
  }

  std::size_t with_connection_calls{0};
};

PendingChunkDeletion pending(std::string hash, std::string token, int retry = 0) {
  return {.chunk_hash = std::move(hash), .claim_token = std::move(token), .retry_count = retry};
}

void wait_for_cleanup_waiter(const ChunkLifecycleCoordinator& coordinator) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (coordinator.cleanup_waiters() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  ASSERT_GT(coordinator.cleanup_waiters(), 0U);
}

TEST(ChunkLifecycleCoordinatorTest, RegisteredCleanupWaiterPrecedesLaterUpload) {
  ChunkLifecycleCoordinator coordinator;
  std::optional<ChunkLifecycleCoordinator::UploadGuard> first_upload{coordinator.acquire_upload_guard()};
  std::mutex mutex;
  std::condition_variable condition;
  bool release_cleanup = false;
  std::atomic<int> acquisition_order{0};
  std::atomic<int> cleanup_order{0};
  std::atomic<int> second_upload_order{0};

  auto cleanup = std::async(std::launch::async, [&] {
    auto guard = coordinator.acquire_cleanup_guard();
    cleanup_order.store(++acquisition_order);
    std::unique_lock lock{mutex};
    condition.wait(lock, [&] { return release_cleanup; });
  });
  wait_for_cleanup_waiter(coordinator);

  auto second_upload = std::async(std::launch::async, [&] {
    auto guard = coordinator.acquire_upload_guard();
    second_upload_order.store(++acquisition_order);
  });

  first_upload = {};
  while (cleanup_order.load() == 0) std::this_thread::yield();
  EXPECT_EQ(second_upload_order.load(), 0);
  {
    std::lock_guard lock{mutex};
    release_cleanup = true;
  }
  condition.notify_one();
  cleanup.get();
  second_upload.get();

  EXPECT_EQ(cleanup_order.load(), 1);
  EXPECT_EQ(second_upload_order.load(), 2);
}

TEST(ChunkLifecycleCoordinatorTest, GuardsAreMoveSafeAndReleaseOnDestruction) {
  ChunkLifecycleCoordinator coordinator;
  std::optional<ChunkLifecycleCoordinator::UploadGuard> held_upload;
  {
    auto upload = coordinator.acquire_upload_guard();
    held_upload.emplace(std::move(upload));
  }
  std::atomic<bool> cleanup_acquired{false};
  auto cleanup = std::async(std::launch::async, [&] {
    auto guard = coordinator.acquire_cleanup_guard();
    cleanup_acquired.store(true);
  });
  wait_for_cleanup_waiter(coordinator);
  EXPECT_FALSE(cleanup_acquired.load());
  held_upload.reset();
  cleanup.get();
  EXPECT_TRUE(cleanup_acquired.load());

  std::optional<ChunkLifecycleCoordinator::CleanupGuard> held_cleanup;
  {
    auto guard = coordinator.acquire_cleanup_guard();
    held_cleanup.emplace(std::move(guard));
  }
  std::atomic<bool> setup_started{false};
  std::atomic<bool> upload_acquired{false};
  auto upload = std::async(std::launch::async, [&] {
    setup_started.store(true);
    setup_started.notify_one();
    auto guard = coordinator.acquire_upload_guard();
    upload_acquired.store(true);
  });
  setup_started.wait(false);
  EXPECT_FALSE(upload_acquired.load());
  held_cleanup.reset();
  upload.get();
  EXPECT_TRUE(upload_acquired.load());
}

TEST(ChunkLifecycleCoordinatorTest, ActiveCleanupBlocksUploadSetupAndWrite) {
  ChunkLifecycleCoordinator coordinator;
  std::optional<ChunkLifecycleCoordinator::CleanupGuard> cleanup{coordinator.acquire_cleanup_guard()};
  std::atomic<bool> setup_started{false};
  std::atomic<int> writes{0};

  auto upload = std::async(std::launch::async, [&] {
    setup_started.store(true);
    setup_started.notify_one();
    auto guard = coordinator.acquire_upload_guard();
    ++writes;
  });
  setup_started.wait(false);

  EXPECT_EQ(writes.load(), 0);
  cleanup.reset();
  upload.get();
  EXPECT_EQ(writes.load(), 1);
}

TEST(ChunkLifecycleCoordinatorTest, CleanupPermitCarriesOwnerIdentity) {
  ChunkLifecycleCoordinator first;
  ChunkLifecycleCoordinator second;
  auto guard = first.acquire_cleanup_guard();

  EXPECT_TRUE(guard.permit().belongs_to(first));
  EXPECT_FALSE(guard.permit().belongs_to(second));
}

TEST(ChunkLifecycleCoordinatorTest, RepeatedCleanupAcquireOnSameThreadThrowsInsteadOfDeadlocking) {
  ChunkLifecycleCoordinator coordinator;
  auto guard = coordinator.acquire_cleanup_guard();

  EXPECT_THROW(static_cast<void>(coordinator.acquire_cleanup_guard()), std::logic_error);
}

TEST(ChunkLifecycleCoordinatorTest, CleanupGuardCanBeMovedAgainFromMovedFromGuard) {
  ChunkLifecycleCoordinator coordinator;
  auto original = coordinator.acquire_cleanup_guard();
  auto owner = move_guard(original);
  auto empty = move_guard(original);

  EXPECT_TRUE(owner.permit().belongs_to(coordinator));
  EXPECT_FALSE(empty.permit().belongs_to(coordinator));
}

TEST(ChunkLifecycleCoordinatorTest, MovedFromCleanupPermitCannotAuthorizeCleanupAfterOwnerReleasesLock) {
  ChunkLifecycleCoordinator coordinator;
  std::vector<MockConnection*> connections;
  ObservedDatabasePool database{[&connections]() -> std::unique_ptr<IConnection> {
    auto connection = std::make_unique<MockConnection>();
    connections.push_back(connection.get());
    return connection;
  }};
  DbConfig config;
  config.pool_size = 1;
  ASSERT_TRUE(database.init(config));
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));
  ASSERT_EQ(connections.size(), 1U);
  std::vector<std::string> executed;
  connections.front()->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    executed.push_back(sql);
    return std::optional<int64_t>{1};
  };

  auto source = coordinator.acquire_cleanup_guard();
  {
    auto target = move_guard(source);
    EXPECT_FALSE(source.permit().belongs_to(coordinator));
    EXPECT_TRUE(target.permit().belongs_to(coordinator));

    auto moved_from_again = move_guard(source);
    EXPECT_FALSE(moved_from_again.permit().belongs_to(coordinator));
  }

  const auto deletion = database.delete_file_owned(source.permit(), 7, 42, false);
  EXPECT_EQ(deletion.status, MutationStatus::INVALID_STATE);
  EXPECT_EQ(deletion.detail, "CLEANUP_PERMIT_INVALID");
  EXPECT_EQ(database.with_connection_calls, 0U);
  EXPECT_TRUE(executed.empty());

  StatusFileSystem file_system;
  const auto cleanup = run_pending_chunk_deletions_guarded(database, file_system, source.permit(), 32);
  EXPECT_EQ(cleanup.status, MutationStatus::INVALID_STATE);
  EXPECT_EQ(cleanup.detail, "CLEANUP_PERMIT_INVALID");
  EXPECT_EQ(database.with_connection_calls, 0U);
  EXPECT_TRUE(file_system.paths_empty());
  EXPECT_TRUE(executed.empty());

  EXPECT_NO_THROW(static_cast<void>(coordinator.acquire_upload_guard()));
}

TEST(ChunkLifecycleCoordinatorTest, CleanupThenUploadOnSameThreadThrowsInsteadOfDeadlocking) {
  ChunkLifecycleCoordinator coordinator;
  auto cleanup = coordinator.acquire_cleanup_guard();

  EXPECT_THROW(static_cast<void>(coordinator.acquire_upload_guard()), std::logic_error);
}

TEST(ChunkLifecycleCoordinatorTest, UploadThenCleanupOnSameThreadThrowsInsteadOfDeadlocking) {
  ChunkLifecycleCoordinator coordinator;
  auto upload = coordinator.acquire_upload_guard();

  EXPECT_THROW(static_cast<void>(coordinator.acquire_cleanup_guard()), std::logic_error);
}

TEST(ChunkLifecycleCoordinatorTest, NestedUploadsOnSameThreadMaintainActiveCount) {
  ChunkLifecycleCoordinator coordinator;
  std::optional<ChunkLifecycleCoordinator::UploadGuard> first{coordinator.acquire_upload_guard()};
  EXPECT_EQ(coordinator.active_uploads(), 1U);

  {
    auto second = coordinator.acquire_upload_guard();
    EXPECT_EQ(coordinator.active_uploads(), 2U);
  }
  EXPECT_EQ(coordinator.active_uploads(), 1U);

  first.reset();
  EXPECT_EQ(coordinator.active_uploads(), 0U);
}

TEST(ChunkLifecycleCoordinatorTest, UploadGuardMovedAcrossThreadReleasesAcquisitionThreadCount) {
  ChunkLifecycleCoordinator coordinator;
  auto acquired = coordinator.acquire_upload_guard();
  std::mutex mutex;
  std::condition_variable condition;
  bool moved = false;
  bool release = false;

  std::thread destroyer{[guard = std::move(acquired), &mutex, &condition, &moved, &release]() mutable {
    {
      std::lock_guard lock{mutex};
      moved = true;
    }
    condition.notify_one();

    std::unique_lock lock{mutex};
    condition.wait(lock, [&release] { return release; });
  }};

  {
    std::unique_lock lock{mutex};
    condition.wait(lock, [&moved] { return moved; });
  }
  EXPECT_EQ(coordinator.active_uploads(), 1U);
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  condition.notify_one();
  destroyer.join();

  EXPECT_EQ(coordinator.active_uploads(), 0U);
  EXPECT_NO_THROW(static_cast<void>(coordinator.acquire_cleanup_guard()));
}

TEST(PendingChunkDeletionConsumerTest, GuardedEntryRejectsPermitFromDifferentCanonicalCoordinator) {
  StubPendingDatabase database;
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator canonical;
  ChunkLifecycleCoordinator other;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(canonical));
  auto wrong_guard = other.acquire_cleanup_guard();

  const auto result = run_pending_chunk_deletions_guarded(database, file_system, wrong_guard.permit(), 32);

  EXPECT_EQ(result.status, MutationStatus::INVALID_STATE);
  EXPECT_EQ(result.detail, "CLEANUP_PERMIT_INVALID");
  EXPECT_EQ(database.observed_limit, 0U);
}

TEST(PendingChunkDeletionConsumerTest, DefaultDatabaseDeleteRejectsPermitFromDifferentCoordinator) {
  StubPendingDatabase database;
  ChunkLifecycleCoordinator canonical;
  ChunkLifecycleCoordinator other;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(canonical));
  auto wrong_guard = other.acquire_cleanup_guard();

  const auto result = database.delete_file_owned(wrong_guard.permit(), 7, 42, false);

  EXPECT_EQ(result.status, MutationStatus::INVALID_STATE);
  EXPECT_EQ(result.detail, "CLEANUP_PERMIT_INVALID");
}

TEST(PendingChunkDeletionConsumerTest, DeletedAndMissingChunksAreCompleted) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK,
                           std::vector{pending("deleted", "token-1"), pending("missing", "token-2")},
                           std::nullopt};
  StatusFileSystem file_system;
  file_system.statuses = {ChunkDeleteStatus::DELETED, ChunkDeleteStatus::NOT_FOUND};
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));

  const auto result = run_pending_chunk_deletions(database, file_system, coordinator, 32);

  ASSERT_EQ(result.status, MutationStatus::OK);
  ASSERT_EQ(result.value, 2U);
  EXPECT_EQ(database.observed_limit, 32U);
  EXPECT_EQ(database.completed.size(), 2U);
  EXPECT_TRUE(database.released_errors.empty());
}

TEST(PendingChunkDeletionConsumerTest, StorageErrorIsReleasedWithSanitizedBoundedMessage) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK, std::vector{pending("hash", "token")}, std::nullopt};
  StatusFileSystem file_system;
  file_system.throw_on_delete = true;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));

  const auto result = run_pending_chunk_deletions(database, file_system, coordinator, 32);

  ASSERT_EQ(result.status, MutationStatus::OK);
  EXPECT_EQ(result.value, 1U);
  ASSERT_EQ(database.released_errors.size(), 1U);
  EXPECT_LE(database.released_errors.front().size(), 512U);
  EXPECT_EQ(database.released_errors.front().find("/secret/path"), std::string::npos);
  EXPECT_TRUE(database.completed.empty());
}

TEST(PendingChunkDeletionConsumerTest, ClaimStorageFailureStopsWithoutPhysicalDeletion) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::STORAGE_ERROR, std::nullopt, "CLAIM_FAILED"};
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));

  const auto result = run_pending_chunk_deletions(database, file_system, coordinator, 100);

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  EXPECT_TRUE(file_system.paths_empty());
}

TEST(PendingChunkDeletionConsumerTest, ThrowingClaimIsConvertedToStorageErrorWithoutPhysicalDeletion) {
  StubPendingDatabase database;
  database.throw_on_claim = true;
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));
  MutationResult<std::size_t> result{MutationStatus::STORAGE_ERROR, std::nullopt, std::nullopt};

  EXPECT_NO_THROW(result = run_pending_chunk_deletions(database, file_system, coordinator, 2));

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  EXPECT_EQ(result.detail, "PENDING_CLAIM_FAILED");
  EXPECT_TRUE(file_system.paths_empty());
  EXPECT_TRUE(database.released.empty());
}

TEST(PendingChunkDeletionConsumerTest, ThrowingReferenceLookupReleasesCurrentAndRemainingClaimsWithoutDeletingThem) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK,
                           std::vector{pending("current", "token-1"), pending("later", "token-2")},
                           std::nullopt};
  database.throw_on_reference = true;
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));
  MutationResult<std::size_t> result{MutationStatus::STORAGE_ERROR, std::nullopt, std::nullopt};

  EXPECT_NO_THROW(result = run_pending_chunk_deletions(database, file_system, coordinator, 2));

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  ASSERT_TRUE(result.value);
  EXPECT_EQ(*result.value, 0U);
  EXPECT_EQ(database.reference_checks, (std::vector<std::string>{"current"}));
  EXPECT_EQ(database.released,
            (std::vector<std::pair<std::string, std::string>>{{"current", "token-1"}, {"later", "token-2"}}));
  EXPECT_TRUE(file_system.paths_empty());
}

TEST(PendingChunkDeletionConsumerTest, ThrowingCompleteReleasesCurrentAndRemainingClaimsWithoutDeletingLaterItem) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK,
                           std::vector{pending("current", "token-1"), pending("later", "token-2")},
                           std::nullopt};
  database.throw_on_complete = true;
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));
  MutationResult<std::size_t> result{MutationStatus::STORAGE_ERROR, std::nullopt, std::nullopt};

  EXPECT_NO_THROW(result = run_pending_chunk_deletions(database, file_system, coordinator, 2));

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  ASSERT_TRUE(result.value);
  EXPECT_EQ(*result.value, 0U);
  EXPECT_EQ(database.completed, (std::vector<std::pair<std::string, std::string>>{{"current", "token-1"}}));
  EXPECT_EQ(database.released,
            (std::vector<std::pair<std::string, std::string>>{{"current", "token-1"}, {"later", "token-2"}}));
  EXPECT_EQ(file_system.paths, (std::vector<std::string>{"chunks/current"}));
}

TEST(PendingChunkDeletionConsumerTest, ThrowingCancelReleasesCurrentAndRemainingClaimsWithoutPhysicalDeletion) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK,
                           std::vector{pending("current", "token-1"), pending("later", "token-2")},
                           std::nullopt};
  database.reference_result = {LookupStatus::FOUND, true};
  database.throw_on_cancel = true;
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));
  MutationResult<std::size_t> result{MutationStatus::STORAGE_ERROR, std::nullopt, std::nullopt};

  EXPECT_NO_THROW(result = run_pending_chunk_deletions(database, file_system, coordinator, 2));

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  ASSERT_TRUE(result.value);
  EXPECT_EQ(*result.value, 0U);
  EXPECT_EQ(database.cancelled, (std::vector<std::pair<std::string, std::string>>{{"current", "token-1"}}));
  EXPECT_EQ(database.released,
            (std::vector<std::pair<std::string, std::string>>{{"current", "token-1"}, {"later", "token-2"}}));
  EXPECT_TRUE(file_system.paths_empty());
}

TEST(PendingChunkDeletionConsumerTest, ThrowingReleaseStillAttemptsEveryRemainingClaimWithoutPhysicalDeletion) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK,
                           std::vector{pending("current", "token-1"), pending("later", "token-2")},
                           std::nullopt};
  database.throw_on_release = true;
  StatusFileSystem file_system;
  file_system.status = ChunkDeleteStatus::ERROR;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));
  MutationResult<std::size_t> result{MutationStatus::STORAGE_ERROR, std::nullopt, std::nullopt};

  EXPECT_NO_THROW(result = run_pending_chunk_deletions(database, file_system, coordinator, 2));

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  ASSERT_TRUE(result.value);
  EXPECT_EQ(*result.value, 0U);
  EXPECT_EQ(database.released,
            (std::vector<std::pair<std::string, std::string>>{{"current", "token-1"}, {"later", "token-2"}}));
  EXPECT_EQ(file_system.paths, (std::vector<std::string>{"chunks/current"}));
}

TEST(PendingChunkDeletionConsumerTest, FullyCancelledBatchStillAllowsStartupLoopToClaimLaterDueRows) {
  StubPendingDatabase database;
  database.claim_results = {
    {MutationStatus::OK, std::vector{pending("reused-a", "token-1"), pending("reused-b", "token-2")}, std::nullopt},
    {MutationStatus::OK, std::vector{pending("due", "token-3")}, std::nullopt},
    {MutationStatus::OK, std::vector<PendingChunkDeletion>{}, std::nullopt},
  };
  database.reference_results = {{LookupStatus::FOUND, true}, {LookupStatus::FOUND, true}, {LookupStatus::FOUND, false}};
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));

  std::size_t batches = 0;
  for (;;) {
    const auto result = run_pending_chunk_deletions(database, file_system, coordinator, 2);
    ASSERT_EQ(result.status, MutationStatus::OK);
    ASSERT_TRUE(result.value);
    if (*result.value == 0)
      break;
    ++batches;
  }

  EXPECT_EQ(batches, 2U);
  EXPECT_EQ(database.observed_limit, 2U);
  EXPECT_EQ(database.cancelled.size(), 2U);
  EXPECT_EQ(database.completed, (std::vector<std::pair<std::string, std::string>>{{"due", "token-3"}}));
}

TEST(PendingChunkDeletionConsumerTest, CompleteFailureIsPropagated) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK, std::vector{pending("hash", "token")}, std::nullopt};
  database.complete_result = {MutationStatus::STORAGE_ERROR, std::nullopt, "COMPLETE_FAILED"};
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));

  const auto result = run_pending_chunk_deletions(database, file_system, coordinator, 1);

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  EXPECT_EQ(result.detail, "COMPLETE_FAILED");
}

TEST(PendingChunkDeletionConsumerTest, ReferenceLookupFailureReleasesCurrentAndRemainingClaimsWithoutDeletingThem) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK,
                           std::vector{pending("completed", "token-1"),
                                       pending("lookup-failed", "token-2"),
                                       pending("unprocessed", "token-3")},
                           std::nullopt};
  database.reference_results = {{LookupStatus::FOUND, false}, {LookupStatus::STORAGE_ERROR, std::nullopt}};
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));

  const auto result = run_pending_chunk_deletions(database, file_system, coordinator, 3);

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  ASSERT_TRUE(result.value);
  EXPECT_EQ(*result.value, 1U);
  EXPECT_EQ(result.detail, "CHUNK_REFERENCE_LOOKUP_FAILED");
  EXPECT_EQ(database.completed, (std::vector<std::pair<std::string, std::string>>{{"completed", "token-1"}}));
  EXPECT_EQ(database.released,
            (std::vector<std::pair<std::string, std::string>>{{"lookup-failed", "token-2"},
                                                              {"unprocessed", "token-3"}}));
  EXPECT_EQ(database.released_errors,
            (std::vector<std::string>{"chunk reference lookup failed", "chunk reference lookup failed"}));
  EXPECT_EQ(file_system.paths, (std::vector<std::string>{"chunks/completed"}));
}

TEST(PendingChunkDeletionConsumerTest, CompleteFailureReleasesCurrentAndRemainingClaimsWithoutDeletingLaterItems) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK,
                           std::vector{pending("completed", "token-1"),
                                       pending("complete-failed", "token-2"),
                                       pending("unprocessed", "token-3")},
                           std::nullopt};
  database.reference_results = {{LookupStatus::FOUND, false},
                                {LookupStatus::FOUND, false},
                                {LookupStatus::FOUND, false}};
  database.complete_results = {{MutationStatus::OK, std::monostate{}, std::nullopt},
                               {MutationStatus::STORAGE_ERROR, std::nullopt, "COMPLETE_FAILED"}};
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));

  const auto result = run_pending_chunk_deletions(database, file_system, coordinator, 3);

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  ASSERT_TRUE(result.value);
  EXPECT_EQ(*result.value, 1U);
  EXPECT_EQ(result.detail, "COMPLETE_FAILED");
  EXPECT_EQ(database.completed,
            (std::vector<std::pair<std::string, std::string>>{{"completed", "token-1"},
                                                              {"complete-failed", "token-2"}}));
  EXPECT_EQ(database.released,
            (std::vector<std::pair<std::string, std::string>>{{"complete-failed", "token-2"},
                                                              {"unprocessed", "token-3"}}));
  EXPECT_EQ(database.released_errors,
            (std::vector<std::string>{"pending chunk deletion completion failed",
                                      "pending chunk deletion completion failed"}));
  EXPECT_EQ(file_system.paths, (std::vector<std::string>{"chunks/completed", "chunks/complete-failed"}));
}

TEST(PendingChunkDeletionConsumerTest, CancelFailureReleasesCurrentAndRemainingClaimsWithoutPhysicalDeletion) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK,
                           std::vector{pending("cancelled", "token-1"),
                                       pending("cancel-failed", "token-2"),
                                       pending("unprocessed", "token-3")},
                           std::nullopt};
  database.reference_results = {{LookupStatus::FOUND, true}, {LookupStatus::FOUND, true}};
  database.cancel_results = {{MutationStatus::OK, std::monostate{}, std::nullopt},
                             {MutationStatus::CONFLICT, std::nullopt, "CANCEL_FAILED"}};
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));

  const auto result = run_pending_chunk_deletions(database, file_system, coordinator, 3);

  EXPECT_EQ(result.status, MutationStatus::CONFLICT);
  ASSERT_TRUE(result.value);
  EXPECT_EQ(*result.value, 1U);
  EXPECT_EQ(result.detail, "CANCEL_FAILED");
  EXPECT_EQ(database.cancelled,
            (std::vector<std::pair<std::string, std::string>>{{"cancelled", "token-1"}, {"cancel-failed", "token-2"}}));
  EXPECT_EQ(database.released,
            (std::vector<std::pair<std::string, std::string>>{{"cancel-failed", "token-2"},
                                                              {"unprocessed", "token-3"}}));
  EXPECT_EQ(database.released_errors,
            (std::vector<std::string>{"pending chunk deletion cancellation failed",
                                      "pending chunk deletion cancellation failed"}));
  EXPECT_TRUE(file_system.paths_empty());
}

TEST(PendingChunkDeletionConsumerTest, ReleaseFailureStillAttemptsEveryRemainingClaimAndReturnsStorageError) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK,
                           std::vector{pending("cancelled", "token-1"),
                                       pending("cancel-failed", "token-2"),
                                       pending("unprocessed", "token-3")},
                           std::nullopt};
  database.reference_results = {{LookupStatus::FOUND, true}, {LookupStatus::FOUND, true}};
  database.cancel_results = {{MutationStatus::OK, std::monostate{}, std::nullopt},
                             {MutationStatus::CONFLICT, std::nullopt, "CANCEL_FAILED"}};
  database.release_results = {{MutationStatus::NOT_FOUND, std::nullopt, "RELEASE_CURRENT_FAILED"},
                              {MutationStatus::OK, std::monostate{}, std::nullopt}};
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));

  const auto result = run_pending_chunk_deletions(database, file_system, coordinator, 3);

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  ASSERT_TRUE(result.value);
  EXPECT_EQ(*result.value, 1U);
  EXPECT_EQ(result.detail, "RELEASE_CURRENT_FAILED");
  EXPECT_EQ(database.released,
            (std::vector<std::pair<std::string, std::string>>{{"cancel-failed", "token-2"},
                                                              {"unprocessed", "token-3"}}));
  EXPECT_TRUE(file_system.paths_empty());
}

TEST(PendingChunkDeletionConsumerTest, ReleaseFailureIsPropagated) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK, std::vector{pending("hash", "token")}, std::nullopt};
  database.release_result = {MutationStatus::NOT_FOUND, std::nullopt, "RELEASE_FAILED"};
  StatusFileSystem file_system;
  file_system.status = ChunkDeleteStatus::ERROR;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));

  const auto result = run_pending_chunk_deletions(database, file_system, coordinator, 1);

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  EXPECT_EQ(result.detail, "RELEASE_FAILED");
}

TEST(PendingChunkDeletionConsumerTest, CancelFailureIsPropagated) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK, std::vector{pending("hash", "token")}, std::nullopt};
  database.reference_result = {LookupStatus::FOUND, true};
  database.cancel_result = {MutationStatus::CONFLICT, std::nullopt, "CANCEL_FAILED"};
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));

  const auto result = run_pending_chunk_deletions(database, file_system, coordinator, 1);

  EXPECT_EQ(result.status, MutationStatus::CONFLICT);
  EXPECT_EQ(result.detail, "CANCEL_FAILED");
}

TEST(PendingChunkDeletionConsumerTest, UploadGuardBlocksPhysicalDeletionUntilDatabaseReferenceIsVisible) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK, std::vector{pending("hash", "token")}, std::nullopt};
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));
  std::optional<ChunkLifecycleCoordinator::UploadGuard> upload_guard{coordinator.acquire_upload_guard()};

  auto future =
    std::async(std::launch::async, [&] { return run_pending_chunk_deletions(database, file_system, coordinator, 32); });

  wait_for_cleanup_waiter(coordinator);
  EXPECT_EQ(future.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
  EXPECT_TRUE(file_system.paths_empty());

  database.reference_result = {LookupStatus::FOUND, true};
  upload_guard.reset();
  const auto result = future.get();

  EXPECT_EQ(result.status, MutationStatus::OK);
  EXPECT_TRUE(file_system.paths_empty());
  EXPECT_EQ(database.cancelled, (std::vector<std::pair<std::string, std::string>>{{"hash", "token"}}));
  EXPECT_TRUE(database.completed.empty());
}

TEST(PendingChunkDeletionConsumerTest, ReferenceLookupFailureStopsWithoutPhysicalDeletion) {
  StubPendingDatabase database;
  database.claim_result = {MutationStatus::OK, std::vector{pending("hash", "token")}, std::nullopt};
  database.reference_result = {LookupStatus::STORAGE_ERROR, std::nullopt};
  StatusFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));

  const auto result = run_pending_chunk_deletions(database, file_system, coordinator, 32);

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  EXPECT_TRUE(file_system.paths_empty());
}

TEST(PendingChunkDeletionModelTest, FileDeletionAndClaimDefaultsAreStable) {
  FileDeletionPlan plan;
  PendingChunkDeletion deletion;
  EXPECT_EQ(plan.file_id, 0);
  EXPECT_EQ(plan.queued_chunk_count, 0U);
  EXPECT_TRUE(deletion.chunk_hash.empty());
  EXPECT_TRUE(deletion.claim_token.empty());
  EXPECT_EQ(deletion.retry_count, 0);
}

} // namespace
} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
