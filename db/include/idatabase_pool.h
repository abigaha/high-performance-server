#pragma once

#include "chunk_lifecycle_coordinator.h"
#include "db_config.h"
#include "models.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hps {

class IConnection;

class IDatabasePool {
public:
  virtual ~IDatabasePool() = default;

  virtual bool init(const DbConfig& config) = 0;
  virtual void close() = 0;

  bool bind_chunk_lifecycle_coordinator(const ChunkLifecycleCoordinator& coordinator) noexcept {
    std::lock_guard lock{chunk_lifecycle_mutex_};
    if (chunk_lifecycle_coordinator_ == nullptr) {
      chunk_lifecycle_coordinator_ = &coordinator;
      return true;
    }
    return chunk_lifecycle_coordinator_ == &coordinator;
  }

  bool accepts_cleanup_permit(const ChunkLifecycleCoordinator::CleanupPermit& permit) const noexcept {
    std::lock_guard lock{chunk_lifecycle_mutex_};
    return chunk_lifecycle_coordinator_ != nullptr && permit.belongs_to(*chunk_lifecycle_coordinator_);
  }

  bool is_chunk_lifecycle_coordinator_bound_to(const ChunkLifecycleCoordinator& coordinator) const noexcept {
    std::lock_guard lock{chunk_lifecycle_mutex_};
    return chunk_lifecycle_coordinator_ == &coordinator;
  }

  virtual bool with_connection(const std::function<bool(IConnection&)>& operation) {
    (void)operation;
    return false;
  }

  // === 用户 ===
  virtual LookupResult<User> get_user_result(int64_t user_id) = 0;

  virtual std::optional<User> get_user(int64_t user_id) { return get_user_result(user_id).value; }

  virtual MutationResult<std::monostate> create_user(const User& user) = 0;
  virtual MutationResult<std::monostate> update_user(const User& user) = 0;
  virtual bool username_exists(const std::string& username) = 0;

  virtual LookupResult<User> get_admin_user_result() { return {LookupStatus::STORAGE_ERROR, std::nullopt}; }

  virtual LookupResult<User> get_user_by_username_result(const std::string& username) {
    (void)username;
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }

  virtual LookupResult<User> get_user_by_email_result(const std::string& email) {
    (void)email;
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }

  virtual MutationResult<std::monostate> create_admin_user(const User& user) {
    (void)user;
    return {};
  }

  virtual MutationResult<std::monostate> update_admin_credentials(const User& user) {
    (void)user;
    return {};
  }

  virtual MutationResult<User> grant_or_extend_vip(int64_t user_id,
                                                   int duration_days,
                                                   std::chrono::system_clock::time_point now) {
    (void)user_id;
    (void)duration_days;
    (void)now;
    return {};
  }

  virtual MutationResult<User> revoke_vip(int64_t user_id) {
    (void)user_id;
    return {};
  }

  virtual LookupResult<AdminUserPage> list_admin_users(const std::string& query, int offset, int limit) {
    (void)query;
    (void)offset;
    (void)limit;
    return {};
  }

  // === 文件记录 ===
  // 成功时返回新建或同哈希已有记录的 file_id。
  virtual std::optional<int64_t> store_file_record(const FileRecord& record) = 0;
  virtual std::optional<FileRecord> get_file_record(int64_t file_id) = 0;

  virtual LookupResult<FileRecord> get_file_record_result(int64_t file_id) {
    (void)file_id;
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }

  virtual std::optional<FileRecord> get_file_record_by_hash(const std::string& hash) = 0;
  virtual std::vector<FileRecord> search_files(const std::string& name_pattern, int offset, int limit) = 0;
  virtual std::vector<FileRecord> search_files_ext(const std::string& name_pattern,
                                                   const std::string& type_filter,
                                                   int offset,
                                                   int limit,
                                                   int& out_total) = 0;

  virtual LookupResult<FilePage> list_files(const std::string& name_pattern,
                                            const std::string& type_filter,
                                            int offset,
                                            int limit) {
    (void)name_pattern;
    (void)type_filter;
    (void)offset;
    (void)limit;
    return {};
  }

  virtual bool update_file_record(const FileRecord& record) = 0;

  virtual MutationResult<FileDeletionPlan> delete_file_owned(const ChunkLifecycleCoordinator::CleanupPermit& permit,
                                                             int64_t file_id,
                                                             int64_t actor_id,
                                                             bool can_delete_any) {
    (void)file_id;
    (void)actor_id;
    (void)can_delete_any;
    if (!accepts_cleanup_permit(permit)) {
      return {MutationStatus::INVALID_STATE, std::nullopt, "CLEANUP_PERMIT_INVALID"};
    }
    return {};
  }

  virtual MutationResult<std::vector<PendingChunkDeletion>> claim_pending_chunk_deletions(
    std::size_t limit,
    std::chrono::system_clock::time_point stale_before) {
    (void)limit;
    (void)stale_before;
    return {};
  }

  virtual MutationResult<std::monostate> complete_pending_chunk_deletion(const std::string& chunk_hash,
                                                                         const std::string& claim_token) {
    (void)chunk_hash;
    (void)claim_token;
    return {};
  }

  virtual MutationResult<std::monostate> release_pending_chunk_deletion(const std::string& chunk_hash,
                                                                        const std::string& claim_token,
                                                                        const std::string& last_error) {
    (void)chunk_hash;
    (void)claim_token;
    (void)last_error;
    return {};
  }

  virtual LookupResult<bool> has_chunk_references(const std::string& chunk_hash) {
    (void)chunk_hash;
    return {};
  }

  virtual MutationResult<std::monostate> cancel_pending_chunk_deletion(const std::string& chunk_hash,
                                                                       const std::string& claim_token) {
    (void)chunk_hash;
    (void)claim_token;
    return {};
  }

  // === 文件分片 ===
  virtual bool store_file_chunks(const std::vector<FileChunkRecord>& chunks) = 0;
  virtual std::vector<FileChunkRecord> get_file_chunks(const std::string& file_hash) = 0;
  virtual bool chunk_exists(const std::string& chunk_hash) = 0;

  // === 认证 ===
  virtual LookupResult<AuthUser> get_auth_user_result(const std::string& username) = 0;

  virtual std::optional<AuthUser> get_auth_user(const std::string& username) {
    return get_auth_user_result(username).value;
  }

  virtual bool verify_password(const std::string& username, const std::string& password) = 0;

  // === 音乐库 ===
  virtual std::vector<MusicMeta> list_music_library(const std::string& search,
                                                    int offset,
                                                    int limit,
                                                    int& out_total) = 0;
  virtual std::optional<MusicMeta> get_music_meta(int64_t music_id) = 0;
  virtual std::optional<MusicMeta> get_music_by_file_id(int64_t file_id) = 0;
  virtual int64_t create_music_meta(const MusicMeta& meta) = 0;
  virtual bool update_music_meta(const MusicMeta& meta) = 0;
  virtual bool delete_music_meta(int64_t music_id) = 0;

  // === 歌单 ===
  virtual MutationResult<std::vector<Playlist>> get_user_playlists(int64_t user_id, int64_t actor_id) = 0;
  virtual MutationResult<Playlist> create_playlist(const Playlist& playlist, int64_t actor_id) = 0;
  virtual MutationResult<Playlist> update_playlist(int64_t playlist_id,
                                                   int64_t actor_id,
                                                   const std::string& name,
                                                   const std::string& description) = 0;
  virtual MutationResult<std::monostate> delete_playlist(int64_t playlist_id, int64_t actor_id) = 0;
  virtual MutationResult<std::vector<PlaylistItem>> get_playlist_items(int64_t playlist_id, int64_t actor_id) = 0;
  virtual MutationResult<std::monostate> add_playlist_item(int64_t playlist_id, int64_t actor_id, int64_t music_id) = 0;
  virtual MutationResult<std::monostate> remove_playlist_item(int64_t playlist_id,
                                                              int64_t actor_id,
                                                              int64_t music_id) = 0;
  virtual MutationResult<std::monostate> reorder_playlist_items(int64_t playlist_id,
                                                                int64_t actor_id,
                                                                const std::vector<int64_t>& music_ids) = 0;

private:
  mutable std::mutex chunk_lifecycle_mutex_;
  const ChunkLifecycleCoordinator* chunk_lifecycle_coordinator_{nullptr};
};

} // namespace hps
