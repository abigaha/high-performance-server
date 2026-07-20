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

  std::optional<User> get_user(int64_t user_id) override;
  bool create_user(const User& user) override;
  bool update_user(const User& user) override;
  bool username_exists(const std::string& username) override;

  std::optional<int64_t> store_file_record(const FileRecord& record) override;
  std::optional<FileRecord> get_file_record(int64_t file_id) override;
  std::optional<FileRecord> get_file_record_by_hash(const std::string& hash) override;
  std::vector<FileRecord> search_files(const std::string& name_pattern, int offset, int limit) override;
  std::vector<FileRecord> search_files_ext(const std::string& name_pattern,
                                           const std::string& type_filter,
                                           int offset,
                                           int limit,
                                           int& out_total) override;
  bool delete_file_record(int64_t file_id) override;
  bool update_file_record(const FileRecord& record) override;

  bool store_file_chunks(const std::vector<FileChunkRecord>& chunks) override;
  std::vector<FileChunkRecord> get_file_chunks(const std::string& file_hash) override;
  bool chunk_exists(const std::string& chunk_hash) override;

  std::optional<AuthUser> get_auth_user(const std::string& username) override;
  bool verify_password(const std::string& username, const std::string& password) override;

  std::vector<MusicMeta> list_music_library(const std::string& search, int offset, int limit, int& out_total) override;
  std::optional<MusicMeta> get_music_meta(int64_t music_id) override;
  std::optional<MusicMeta> get_music_by_file_id(int64_t file_id) override;
  int64_t create_music_meta(const MusicMeta& meta) override;
  bool update_music_meta(const MusicMeta& meta) override;
  bool delete_music_meta(int64_t music_id) override;

  std::vector<Playlist> get_user_playlists(int64_t user_id) override;
  int64_t create_playlist(const Playlist& pl) override;
  bool delete_playlist(int64_t playlist_id) override;
  std::vector<PlaylistItem> get_playlist_items(int64_t playlist_id) override;
  bool add_playlist_item(int64_t playlist_id, int64_t music_id) override;
  bool remove_playlist_item(int64_t playlist_id, int64_t music_id) override;
  bool reorder_playlist_items(int64_t playlist_id, const std::vector<int64_t>& music_ids) override;

protected:
  std::unique_ptr<IConnection> get_connection();
  void release_connection(std::unique_ptr<IConnection> conn);

private:
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
