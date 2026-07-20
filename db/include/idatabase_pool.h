#pragma once

#include "db_config.h"
#include "models.h"

#include <optional>
#include <string>
#include <vector>

namespace hps {

class IDatabasePool {
public:
  virtual ~IDatabasePool() = default;

  virtual bool init(const DbConfig& config) = 0;
  virtual void close() = 0;

  // === 用户 ===
  virtual std::optional<User> get_user(int64_t user_id) = 0;
  virtual bool create_user(const User& user) = 0;
  virtual bool update_user(const User& user) = 0;
  virtual bool username_exists(const std::string& username) = 0;

  // === 文件记录 ===
  // 成功时返回新建或同哈希已有记录的 file_id。
  virtual std::optional<int64_t> store_file_record(const FileRecord& record) = 0;
  virtual std::optional<FileRecord> get_file_record(int64_t file_id) = 0;
  virtual std::optional<FileRecord> get_file_record_by_hash(const std::string& hash) = 0;
  virtual std::vector<FileRecord> search_files(const std::string& name_pattern, int offset, int limit) = 0;
  virtual std::vector<FileRecord> search_files_ext(const std::string& name_pattern,
                                                   const std::string& type_filter,
                                                   int offset,
                                                   int limit,
                                                   int& out_total) = 0;
  virtual bool delete_file_record(int64_t file_id) = 0;
  virtual bool update_file_record(const FileRecord& record) = 0;

  // === 文件分片 ===
  virtual bool store_file_chunks(const std::vector<FileChunkRecord>& chunks) = 0;
  virtual std::vector<FileChunkRecord> get_file_chunks(const std::string& file_hash) = 0;
  virtual bool chunk_exists(const std::string& chunk_hash) = 0;

  // === 认证 ===
  virtual std::optional<AuthUser> get_auth_user(const std::string& username) = 0;
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
  virtual std::vector<Playlist> get_user_playlists(int64_t user_id) = 0;
  virtual int64_t create_playlist(const Playlist& pl) = 0;
  virtual bool delete_playlist(int64_t playlist_id) = 0;
  virtual std::vector<PlaylistItem> get_playlist_items(int64_t playlist_id) = 0;
  virtual bool add_playlist_item(int64_t playlist_id, int64_t music_id) = 0;
  virtual bool remove_playlist_item(int64_t playlist_id, int64_t music_id) = 0;
  virtual bool reorder_playlist_items(int64_t playlist_id, const std::vector<int64_t>& music_ids) = 0;
};

} // namespace hps
