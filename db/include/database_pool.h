#pragma once

#include "iconnection.h"
#include "idatabase_pool.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace hps {

class DatabasePool : public IDatabasePool {
public:
  using ConnectionFactory = std::function<std::unique_ptr<IConnection>()>;

  explicit DatabasePool(ConnectionFactory factory = nullptr);
  ~DatabasePool() override;

  DatabasePool(const DatabasePool&) = delete;
  DatabasePool& operator=(const DatabasePool&) = delete;
  DatabasePool(DatabasePool&&) = delete;
  DatabasePool& operator=(DatabasePool&&) = delete;

  bool init(const DbConfig& config) override;
  void close() override;
  bool with_connection(const std::function<bool(IConnection&)>& operation) override;

  LookupResult<User> get_user_result(int64_t user_id) override;
  MutationResult<std::monostate> create_user(const User& user) override;
  MutationResult<std::monostate> update_user(const User& user) override;
  bool username_exists(const std::string& username) override;
  LookupResult<User> get_admin_user_result() override;
  LookupResult<User> get_user_by_username_result(const std::string& username) override;
  LookupResult<User> get_user_by_email_result(const std::string& email) override;
  MutationResult<std::monostate> create_admin_user(const User& user) override;
  MutationResult<std::monostate> update_admin_credentials(const User& user) override;
  MutationResult<User> grant_or_extend_vip(int64_t user_id,
                                           int duration_days,
                                           std::chrono::system_clock::time_point now) override;
  MutationResult<User> revoke_vip(int64_t user_id) override;
  LookupResult<AdminUserPage> list_admin_users(const std::string& query, int offset, int limit) override;

  std::optional<int64_t> store_file_record(const FileRecord& record) override;
  std::optional<FileRecord> get_file_record(int64_t file_id) override;
  LookupResult<FileRecord> get_file_record_result(int64_t file_id) override;
  std::optional<FileRecord> get_file_record_by_hash(const std::string& hash) override;
  std::vector<FileRecord> search_files(const std::string& name_pattern, int offset, int limit) override;
  std::vector<FileRecord> search_files_ext(const std::string& name_pattern,
                                           const std::string& type_filter,
                                           int offset,
                                           int limit,
                                           int& out_total) override;
  LookupResult<FilePage> list_files(const std::string& name_pattern,
                                    const std::string& type_filter,
                                    int offset,
                                    int limit) override;
  bool update_file_record(const FileRecord& record) override;
  MutationResult<FileDeletionPlan> delete_file_owned(const ChunkLifecycleCoordinator::CleanupPermit& permit,
                                                     int64_t file_id,
                                                     int64_t actor_id,
                                                     bool can_delete_any) override;
  MutationResult<std::vector<PendingChunkDeletion>> claim_pending_chunk_deletions(
    std::size_t limit,
    std::chrono::system_clock::time_point stale_before) override;
  MutationResult<std::monostate> complete_pending_chunk_deletion(const std::string& chunk_hash,
                                                                 const std::string& claim_token) override;
  MutationResult<std::monostate> release_pending_chunk_deletion(const std::string& chunk_hash,
                                                                const std::string& claim_token,
                                                                const std::string& last_error) override;
  LookupResult<bool> has_chunk_references(const std::string& chunk_hash) override;
  MutationResult<std::monostate> cancel_pending_chunk_deletion(const std::string& chunk_hash,
                                                               const std::string& claim_token) override;

  bool store_file_chunks(const std::vector<FileChunkRecord>& chunks) override;
  std::vector<FileChunkRecord> get_file_chunks(const std::string& file_hash) override;
  bool chunk_exists(const std::string& chunk_hash) override;

  LookupResult<AuthUser> get_auth_user_result(const std::string& username) override;
  bool verify_password(const std::string& username, const std::string& password) override;

  std::vector<MusicMeta> list_music_library(const std::string& search, int offset, int limit, int& out_total) override;
  std::optional<MusicMeta> get_music_meta(int64_t music_id) override;
  std::optional<MusicMeta> get_music_by_file_id(int64_t file_id) override;
  int64_t create_music_meta(const MusicMeta& meta) override;
  bool update_music_meta(const MusicMeta& meta) override;
  bool delete_music_meta(int64_t music_id) override;

  MutationResult<std::vector<Playlist>> get_user_playlists(int64_t user_id, int64_t actor_id) override;
  MutationResult<Playlist> create_playlist(const Playlist& playlist, int64_t actor_id) override;
  MutationResult<Playlist> update_playlist(int64_t playlist_id,
                                           int64_t actor_id,
                                           const std::string& name,
                                           const std::string& description) override;
  MutationResult<std::monostate> delete_playlist(int64_t playlist_id, int64_t actor_id) override;
  MutationResult<std::vector<PlaylistItem>> get_playlist_items(int64_t playlist_id, int64_t actor_id) override;
  MutationResult<std::monostate> add_playlist_item(int64_t playlist_id, int64_t actor_id, int64_t music_id) override;
  MutationResult<std::monostate> remove_playlist_item(int64_t playlist_id, int64_t actor_id, int64_t music_id) override;
  MutationResult<std::monostate> reorder_playlist_items(int64_t playlist_id,
                                                        int64_t actor_id,
                                                        const std::vector<int64_t>& music_ids) override;

protected:
  std::unique_ptr<IConnection> get_connection();
  void release_connection(std::unique_ptr<IConnection> conn);

private:
  LookupResult<User> lookup_user(const std::string& sql,
                                 const std::vector<std::string>& params,
                                 bool allow_admin_vip_expiry);
  void do_close(); // 非虚，供析构和 close() 共用

  ConnectionFactory factory_;
  DbConfig config_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::unique_ptr<IConnection>> pool_;
  std::atomic<int> active_connections_{0};
  bool closed_{false};
};

} // namespace hps
