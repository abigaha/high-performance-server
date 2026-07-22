#include "database_pool.h"

#include "boost_mysql_connection.h"
#include "models.h"

#include <algorithm>
#include <sstream>
#include <thread>
#include <utility>

namespace hps {

namespace {

std::unique_ptr<IConnection> default_factory() {
  return std::make_unique<BoostMySqlConnection>();
}

std::string build_placeholders(std::size_t count) {
  if (count == 0) {
    return "";
  }
  std::string result;
  result.reserve(count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    if (i > 0) {
      result += ',';
    }
    result += '?';
  }
  return result;
}

} // namespace

DatabasePool::DatabasePool(ConnectionFactory factory) : factory_(factory ? std::move(factory) : default_factory) {}

DatabasePool::~DatabasePool() {
  do_close();
}

void DatabasePool::close() {
  do_close();
}

bool DatabasePool::init(const DbConfig& config) {
  config_ = config;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pool_.empty()) {
    return false;
  }
  for (std::size_t i = 0; i < config_.pool_size; ++i) {
    auto conn = factory_();
    if (!conn->connect(config_)) {
      while (!pool_.empty()) {
        pool_.front()->close();
        pool_.pop();
      }
      return false;
    }
    pool_.push(std::move(conn));
  }
  return true;
}

void DatabasePool::do_close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return;
    }
    closed_ = true;
  }
  cv_.notify_all();

  while (active_connections_.load(std::memory_order_acquire) > 0) {
    std::this_thread::yield();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  while (!pool_.empty()) {
    pool_.front()->close();
    pool_.pop();
  }
}

std::unique_ptr<IConnection> DatabasePool::get_connection() {
  std::unique_lock<std::mutex> lock(mutex_);
  auto timeout = std::chrono::milliseconds(config_.connect_timeout_ms);
  if (!cv_.wait_for(lock, timeout, [this]() { return !pool_.empty() || closed_; })) {
    return nullptr;
  }
  if (closed_) {
    return nullptr;
  }
  auto conn = std::move(pool_.front());
  pool_.pop();
  lock.unlock();

  if (conn->is_open()) {
    if (!conn->ping()) {
      conn->close();
      if (!conn->connect(config_)) {
        return nullptr;
      }
    }
  } else {
    if (!conn->connect(config_)) {
      return nullptr;
    }
  }

  active_connections_.fetch_add(1, std::memory_order_relaxed);
  return conn;
}

void DatabasePool::release_connection(std::unique_ptr<IConnection> conn) {
  if (!conn) {
    return;
  }
  active_connections_.fetch_sub(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!closed_) {
      pool_.push(std::move(conn));
    }
  }
  cv_.notify_one();
}

namespace {

int to_role_int(UserRole role) {
  return static_cast<int>(role);
}

UserRole to_user_role(int v) {
  if (v >= 2) {
    return UserRole::VIP;
  }
  if (v >= 1) {
    return UserRole::NORMAL;
  }
  return UserRole::GUEST;
}

} // namespace

std::optional<User> DatabasePool::get_user(int64_t user_id) {
  auto conn = get_connection();
  if (!conn) {
    return std::nullopt;
  }

  auto result = conn->query("SELECT user_id, username, password_hash, salt, role, email, created_at "
                            "FROM users WHERE user_id = ?",
                            {std::to_string(user_id)});

  release_connection(std::move(conn));

  if (!result || result->rows.empty()) {
    return std::nullopt;
  }

  User u{};
  const auto& row = result->rows[0];
  if (row.size() >= 7) {
    u.user_id = row[0].empty() ? 0 : std::stoll(row[0]);
    u.username = row[1];
    u.password_hash = row[2];
    u.salt = row[3];
    u.role = to_user_role(row[4].empty() ? 0 : std::stoi(row[4]));
    u.email = row[5];
    u.created_at = row[6];
  }
  return u;
}

bool DatabasePool::create_user(const User& user) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected =
    conn->execute("INSERT INTO users (username, password_hash, salt, role, email) VALUES (?, ?, ?, ?, ?)",
                  {user.username, user.password_hash, user.salt, std::to_string(to_role_int(user.role)), user.email});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

std::optional<int64_t> DatabasePool::store_file_record(const FileRecord& record) {
  auto conn = get_connection();
  if (!conn) {
    return std::nullopt;
  }

  auto affected = conn->execute("INSERT INTO file_records "
                                "(file_name, file_hash, file_size, content_type, chunk_size, music_id, uploaded_by) "
                                "VALUES (?, ?, ?, ?, ?, NULLIF(?, '0'), ?) "
                                "ON DUPLICATE KEY UPDATE file_id=LAST_INSERT_ID(file_id), "
                                "file_name=VALUES(file_name), "
                                "file_size=VALUES(file_size), content_type=VALUES(content_type), "
                                "music_id=VALUES(music_id), uploaded_by=VALUES(uploaded_by)",
                                {record.file_name,
                                 record.file_hash,
                                 std::to_string(record.file_size),
                                 record.content_type,
                                 std::to_string(record.chunk_size),
                                 std::to_string(record.music_id),
                                 std::to_string(record.uploaded_by)});

  std::optional<int64_t> file_id;
  if (affected.has_value()) {
    auto last_id = conn->last_insert_id();
    if (last_id > 0) {
      file_id = last_id;
    }
  }

  release_connection(std::move(conn));
  return file_id;
}

std::optional<FileRecord> DatabasePool::get_file_record(int64_t file_id) {
  auto conn = get_connection();
  if (!conn) {
    return std::nullopt;
  }

  auto result = conn->query("SELECT file_id, file_name, file_hash, file_size, content_type, "
                            "chunk_size, created_at, music_id, uploaded_by "
                            "FROM file_records WHERE file_id = ?",
                            {std::to_string(file_id)});

  release_connection(std::move(conn));

  if (!result || result->rows.empty()) {
    return std::nullopt;
  }

  const auto& row = result->rows[0];
  if (row.size() < 9) {
    return std::nullopt;
  }

  FileRecord r{};
  r.file_id = row[0].empty() ? 0 : std::stoll(row[0]);
  r.file_name = row[1];
  r.file_hash = row[2];
  r.file_size = row[3].empty() ? 0 : std::stoull(row[3]);
  r.content_type = row[4];
  r.chunk_size = row[5].empty() ? 2097152 : std::stoi(row[5]);
  r.created_at = row[6];
  r.music_id = row[7].empty() ? 0 : std::stoll(row[7]);
  r.uploaded_by = row[8].empty() ? 0 : std::stoll(row[8]);
  return r;
}

std::optional<FileRecord> DatabasePool::get_file_record_by_hash(const std::string& hash) {
  auto conn = get_connection();
  if (!conn) {
    return std::nullopt;
  }

  auto result = conn->query("SELECT file_id, file_name, file_hash, file_size, content_type, "
                            "chunk_size, created_at, music_id, uploaded_by "
                            "FROM file_records WHERE file_hash = ?",
                            {hash});

  release_connection(std::move(conn));

  if (!result || result->rows.empty()) {
    return std::nullopt;
  }

  const auto& row = result->rows[0];
  if (row.size() < 9) {
    return std::nullopt;
  }

  FileRecord r{};
  r.file_id = row[0].empty() ? 0 : std::stoll(row[0]);
  r.file_name = row[1];
  r.file_hash = row[2];
  r.file_size = row[3].empty() ? 0 : std::stoull(row[3]);
  r.content_type = row[4];
  r.chunk_size = row[5].empty() ? 2097152 : std::stoi(row[5]);
  r.created_at = row[6];
  r.music_id = row[7].empty() ? 0 : std::stoll(row[7]);
  r.uploaded_by = row[8].empty() ? 0 : std::stoll(row[8]);
  return r;
}

std::vector<FileRecord> DatabasePool::search_files(const std::string& name_pattern, int offset, int limit) {
  auto conn = get_connection();
  if (!conn) {
    return {};
  }

  std::string sql = "SELECT file_id, file_name, file_hash, file_size, content_type, "
                    "chunk_size, created_at, music_id, uploaded_by "
                    "FROM file_records";
  std::vector<std::string> params;
  if (!name_pattern.empty()) {
    sql += " WHERE file_name LIKE ?";
    params.push_back("%" + name_pattern + "%");
  }
  sql += " ORDER BY file_id DESC LIMIT ? OFFSET ?";
  params.push_back(std::to_string(limit));
  params.push_back(std::to_string(offset));

  auto result = conn->query(sql, params);

  release_connection(std::move(conn));

  std::vector<FileRecord> records;
  if (!result) {
    return records;
  }
  records.reserve(result->rows.size());
  for (const auto& row : result->rows) {
    if (row.size() < 9) {
      continue;
    }
    FileRecord r{};
    r.file_id = row[0].empty() ? 0 : std::stoll(row[0]);
    r.file_name = row[1];
    r.file_hash = row[2];
    r.file_size = row[3].empty() ? 0 : std::stoull(row[3]);
    r.content_type = row[4];
    r.chunk_size = row[5].empty() ? 2097152 : std::stoi(row[5]);
    r.created_at = row[6];
    r.music_id = row[7].empty() ? 0 : std::stoll(row[7]);
    r.uploaded_by = row[8].empty() ? 0 : std::stoll(row[8]);
    records.push_back(std::move(r));
  }
  return records;
}

bool DatabasePool::store_file_chunks(const std::vector<FileChunkRecord>& chunks) {
  if (chunks.empty()) {
    return true;
  }

  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  bool ok = true;
  for (const auto& c : chunks) {
    auto affected = conn->execute("INSERT IGNORE INTO file_chunks "
                                  "(file_hash, chunk_index, chunk_hash, chunk_offset, chunk_size) "
                                  "VALUES (?, ?, ?, ?, ?)",
                                  {c.file_hash,
                                   std::to_string(c.chunk_index),
                                   c.chunk_hash,
                                   std::to_string(c.chunk_offset),
                                   std::to_string(c.chunk_size)});
    if (!affected.has_value()) {
      ok = false;
      break;
    }
  }

  release_connection(std::move(conn));
  return ok;
}

std::vector<FileChunkRecord> DatabasePool::get_file_chunks(const std::string& file_hash) {
  auto conn = get_connection();
  if (!conn) {
    return {};
  }

  auto result = conn->query("SELECT file_hash, chunk_index, chunk_hash, chunk_offset, chunk_size "
                            "FROM file_chunks WHERE file_hash = ? ORDER BY chunk_index",
                            {file_hash});

  release_connection(std::move(conn));

  std::vector<FileChunkRecord> chunks;
  if (!result) {
    return chunks;
  }
  chunks.reserve(result->rows.size());
  for (const auto& row : result->rows) {
    if (row.size() < 5) {
      continue;
    }
    FileChunkRecord c{};
    c.file_hash = row[0];
    c.chunk_index = row[1].empty() ? 0 : std::stoi(row[1]);
    c.chunk_hash = row[2];
    c.chunk_offset = row[3].empty() ? 0 : std::stoull(row[3]);
    c.chunk_size = row[4].empty() ? 0 : std::stoi(row[4]);
    chunks.push_back(std::move(c));
  }
  return chunks;
}

bool DatabasePool::chunk_exists(const std::string& chunk_hash) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto result = conn->query("SELECT 1 FROM file_chunks WHERE chunk_hash = ? LIMIT 1", {chunk_hash});

  release_connection(std::move(conn));

  return result.has_value() && !result->rows.empty();
}

std::optional<AuthUser> DatabasePool::get_auth_user(const std::string& username) {
  auto conn = get_connection();
  if (!conn) {
    return std::nullopt;
  }

  auto result = conn->query("SELECT user_id, username, role "
                            "FROM users WHERE username = ?",
                            {username});

  release_connection(std::move(conn));

  if (!result || result->rows.empty()) {
    return std::nullopt;
  }

  const auto& row = result->rows[0];
  if (row.size() < 3) {
    return std::nullopt;
  }

  AuthUser u{};
  u.user_id = row[0].empty() ? 0 : std::stoll(row[0]);
  u.username = row[1];
  u.role = to_user_role(row[2].empty() ? 0 : std::stoi(row[2]));
  return u;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool DatabasePool::verify_password(const std::string& username, const std::string& password) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto result = conn->query("SELECT password_hash FROM users WHERE username = ?", {username});

  release_connection(std::move(conn));

  if (!result || result->rows.empty()) {
    return false;
  }

  return !result->rows[0].empty() && result->rows[0][0] == password;
}

bool DatabasePool::update_user(const User& user) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected = conn->execute("UPDATE users SET email = ?, password_hash = ?, salt = ? "
                                "WHERE user_id = ?",
                                {user.email, user.password_hash, user.salt, std::to_string(user.user_id)});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

bool DatabasePool::username_exists(const std::string& username) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto result = conn->query("SELECT 1 FROM users WHERE username = ? LIMIT 1", {username});

  release_connection(std::move(conn));
  return result.has_value() && !result->rows.empty();
}

std::vector<FileRecord> DatabasePool::search_files_ext(const std::string& name_pattern,
                                                       const std::string& type_filter,
                                                       int offset,
                                                       int limit,
                                                       int& out_total) {
  auto conn = get_connection();
  if (!conn) {
    out_total = 0;
    return {};
  }

  std::string where;
  std::vector<std::string> params;
  if (!name_pattern.empty()) {
    where += " AND f.file_name LIKE ?";
    params.push_back("%" + name_pattern + "%");
  }
  if (!type_filter.empty()) {
    where += " AND f.content_type LIKE CONCAT(?, '%')";
    params.push_back(type_filter);
  }

  auto count_params = params;
  auto count_result = conn->query("SELECT COUNT(*) FROM file_records f WHERE 1=1" + where, count_params);
  out_total = 0;
  if (count_result && !count_result->rows.empty() && !count_result->rows[0].empty()) {
    out_total = std::stoi(count_result->rows[0][0]);
  }

  auto query_params = params;
  query_params.push_back(std::to_string(limit));
  query_params.push_back(std::to_string(offset));

  auto result = conn->query("SELECT f.file_id, f.file_name, f.file_hash, f.file_size, "
                            "f.content_type, f.chunk_size, f.created_at, f.music_id, f.uploaded_by "
                            "FROM file_records f WHERE 1=1" +
                              where + " ORDER BY f.file_id DESC LIMIT ? OFFSET ?",
                            query_params);

  release_connection(std::move(conn));

  std::vector<FileRecord> records;
  if (!result) {
    return records;
  }
  records.reserve(result->rows.size());
  for (const auto& row : result->rows) {
    if (row.size() < 9) {
      continue;
    }
    FileRecord r{};
    r.file_id = row[0].empty() ? 0 : std::stoll(row[0]);
    r.file_name = row[1];
    r.file_hash = row[2];
    r.file_size = row[3].empty() ? 0 : std::stoull(row[3]);
    r.content_type = row[4];
    r.chunk_size = row[5].empty() ? 2097152 : std::stoi(row[5]);
    r.created_at = row[6];
    r.music_id = row[7].empty() ? 0 : std::stoll(row[7]);
    r.uploaded_by = row[8].empty() ? 0 : std::stoll(row[8]);
    records.push_back(std::move(r));
  }
  return records;
}

bool DatabasePool::delete_file_record(int64_t file_id) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected = conn->execute("DELETE FROM file_records WHERE file_id = ?", {std::to_string(file_id)});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

bool DatabasePool::update_file_record(const FileRecord& record) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected = conn->execute("UPDATE file_records SET file_name = ?, content_type = ?, "
                                "music_id = NULLIF(?, '0'), uploaded_by = ? WHERE file_id = ?",
                                {record.file_name,
                                 record.content_type,
                                 std::to_string(record.music_id),
                                 std::to_string(record.uploaded_by),
                                 std::to_string(record.file_id)});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

std::vector<MusicMeta> DatabasePool::list_music_library(const std::string& search,
                                                        int offset,
                                                        int limit,
                                                        int& out_total) {
  auto conn = get_connection();
  if (!conn) {
    out_total = 0;
    return {};
  }

  std::vector<std::string> params;
  if (!search.empty()) {
    params = {"%" + search + "%", "%" + search + "%", "%" + search + "%"};
  }

  std::string where = " WHERE EXISTS (SELECT 1 FROM file_records f "
                      "WHERE f.music_id = m.music_id AND f.content_type LIKE 'audio/%')";
  if (!search.empty()) {
    where += " AND (m.title LIKE ? OR m.artist LIKE ? OR m.album LIKE ?)";
  }

  auto count_result = conn->query("SELECT COUNT(*) FROM music_meta m" + where, params);
  out_total = 0;
  if (count_result && !count_result->rows.empty() && !count_result->rows[0].empty()) {
    out_total = std::stoi(count_result->rows[0][0]);
  }

  std::vector<std::string> data_params = params;
  data_params.push_back(std::to_string(limit));
  data_params.push_back(std::to_string(offset));

  auto result = conn->query("SELECT m.music_id, m.title, m.artist, m.album, m.genre, m.duration_sec, "
                            "m.track_number, m.created_at, m.updated_at "
                            "FROM music_meta m" +
                              where + " ORDER BY m.title, m.music_id LIMIT ? OFFSET ?",
                            data_params);

  release_connection(std::move(conn));

  std::vector<MusicMeta> items;
  if (!result) {
    return items;
  }
  items.reserve(result->rows.size());
  for (const auto& row : result->rows) {
    if (row.size() < 9) {
      continue;
    }
    MusicMeta m{};
    m.music_id = row[0].empty() ? 0 : std::stoll(row[0]);
    m.title = row[1];
    m.artist = row[2];
    m.album = row[3];
    m.genre = row[4];
    m.duration_sec = row[5].empty() ? 0 : std::stoi(row[5]);
    m.track_number = row[6].empty() ? 0 : std::stoi(row[6]);
    m.created_at = row[7];
    m.updated_at = row[8];
    items.push_back(std::move(m));
  }
  return items;
}

std::optional<MusicMeta> DatabasePool::get_music_meta(int64_t music_id) {
  auto conn = get_connection();
  if (!conn) {
    return std::nullopt;
  }

  auto result = conn->query("SELECT music_id, title, artist, album, genre, duration_sec, "
                            "track_number, created_at, updated_at "
                            "FROM music_meta WHERE music_id = ?",
                            {std::to_string(music_id)});

  release_connection(std::move(conn));

  if (!result || result->rows.empty()) {
    return std::nullopt;
  }

  const auto& row = result->rows[0];
  if (row.size() < 9) {
    return std::nullopt;
  }

  MusicMeta m{};
  m.music_id = row[0].empty() ? 0 : std::stoll(row[0]);
  m.title = row[1];
  m.artist = row[2];
  m.album = row[3];
  m.genre = row[4];
  m.duration_sec = row[5].empty() ? 0 : std::stoi(row[5]);
  m.track_number = row[6].empty() ? 0 : std::stoi(row[6]);
  m.created_at = row[7];
  m.updated_at = row[8];
  return m;
}

std::optional<MusicMeta> DatabasePool::get_music_by_file_id(int64_t file_id) {
  auto conn = get_connection();
  if (!conn) {
    return std::nullopt;
  }

  auto result = conn->query("SELECT m.music_id, m.title, m.artist, m.album, m.genre, "
                            "m.duration_sec, m.track_number, m.created_at, m.updated_at "
                            "FROM music_meta m "
                            "INNER JOIN file_records f ON f.music_id = m.music_id "
                            "WHERE f.file_id = ?",
                            {std::to_string(file_id)});

  release_connection(std::move(conn));

  if (!result || result->rows.empty()) {
    return std::nullopt;
  }

  const auto& row = result->rows[0];
  if (row.size() < 9) {
    return std::nullopt;
  }

  MusicMeta m{};
  m.music_id = row[0].empty() ? 0 : std::stoll(row[0]);
  m.title = row[1];
  m.artist = row[2];
  m.album = row[3];
  m.genre = row[4];
  m.duration_sec = row[5].empty() ? 0 : std::stoi(row[5]);
  m.track_number = row[6].empty() ? 0 : std::stoi(row[6]);
  m.created_at = row[7];
  m.updated_at = row[8];
  return m;
}

int64_t DatabasePool::create_music_meta(const MusicMeta& meta) {
  auto conn = get_connection();
  if (!conn) {
    return -1;
  }

  auto affected = conn->execute("INSERT INTO music_meta (title, artist, album, genre, "
                                "duration_sec, track_number) "
                                "VALUES (?, ?, ?, ?, ?, ?)",
                                {meta.title,
                                 meta.artist,
                                 meta.album,
                                 meta.genre,
                                 std::to_string(meta.duration_sec),
                                 std::to_string(meta.track_number)});

  int64_t id = -1;
  if (affected.has_value() && *affected > 0) {
    id = conn->last_insert_id();
  }

  release_connection(std::move(conn));
  return id;
}

bool DatabasePool::update_music_meta(const MusicMeta& meta) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected = conn->execute("UPDATE music_meta SET title = ?, artist = ?, album = ?, "
                                "genre = ?, duration_sec = ?, track_number = ? "
                                "WHERE music_id = ?",
                                {meta.title,
                                 meta.artist,
                                 meta.album,
                                 meta.genre,
                                 std::to_string(meta.duration_sec),
                                 std::to_string(meta.track_number),
                                 std::to_string(meta.music_id)});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

bool DatabasePool::delete_music_meta(int64_t music_id) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected =
    conn->execute("UPDATE file_records SET music_id = NULL WHERE music_id = ?", {std::to_string(music_id)});
  if (!affected.has_value()) {
    release_connection(std::move(conn));
    return false;
  }

  affected = conn->execute("DELETE FROM music_meta WHERE music_id = ?", {std::to_string(music_id)});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

std::vector<Playlist> DatabasePool::get_user_playlists(int64_t user_id) {
  auto conn = get_connection();
  if (!conn) {
    return {};
  }

  auto result =
    conn->query("SELECT p.playlist_id, p.user_id, p.name, p.description, "
                "(SELECT COUNT(*) FROM playlist_items pi WHERE pi.playlist_id = p.playlist_id) AS item_count, "
                "p.created_at "
                "FROM user_playlists p WHERE p.user_id = ? ORDER BY p.created_at DESC",
                {std::to_string(user_id)});

  release_connection(std::move(conn));

  std::vector<Playlist> playlists;
  if (!result) {
    return playlists;
  }
  playlists.reserve(result->rows.size());
  for (const auto& row : result->rows) {
    if (row.size() < 6) {
      continue;
    }
    Playlist p{};
    p.playlist_id = row[0].empty() ? 0 : std::stoll(row[0]);
    p.user_id = row[1].empty() ? 0 : std::stoll(row[1]);
    p.name = row[2];
    p.description = row[3];
    p.item_count = row[4].empty() ? 0 : std::stoi(row[4]);
    p.created_at = row[5];
    playlists.push_back(std::move(p));
  }
  return playlists;
}

int64_t DatabasePool::create_playlist(const Playlist& pl) {
  auto conn = get_connection();
  if (!conn) {
    return -1;
  }

  auto affected = conn->execute("INSERT INTO user_playlists (user_id, name, description) "
                                "VALUES (?, ?, ?)",
                                {std::to_string(pl.user_id), pl.name, pl.description});

  int64_t id = -1;
  if (affected.has_value() && *affected > 0) {
    id = conn->last_insert_id();
  }

  release_connection(std::move(conn));
  return id;
}

bool DatabasePool::delete_playlist(int64_t playlist_id) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected = conn->execute("DELETE FROM user_playlists WHERE playlist_id = ?", {std::to_string(playlist_id)});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

std::vector<PlaylistItem> DatabasePool::get_playlist_items(int64_t playlist_id) {
  auto conn = get_connection();
  if (!conn) {
    return {};
  }

  auto result = conn->query("SELECT pi.id, pi.playlist_id, pi.music_id, pi.sort_order, pi.added_at, "
                            "m.title, m.artist, f.file_hash "
                            "FROM playlist_items pi "
                            "STRAIGHT_JOIN music_meta m ON m.music_id = pi.music_id "
                            "LEFT JOIN file_records f ON f.music_id = m.music_id AND f.content_type LIKE 'audio/%' "
                            "WHERE pi.playlist_id = ? ORDER BY pi.sort_order",
                            {std::to_string(playlist_id)});

  release_connection(std::move(conn));

  std::vector<PlaylistItem> items;
  if (!result) {
    return items;
  }
  items.reserve(result->rows.size());
  for (const auto& row : result->rows) {
    if (row.size() < 8) {
      continue;
    }
    PlaylistItem item{};
    item.id = row[0].empty() ? 0 : std::stoll(row[0]);
    item.playlist_id = row[1].empty() ? 0 : std::stoll(row[1]);
    item.music_id = row[2].empty() ? 0 : std::stoll(row[2]);
    item.sort_order = row[3].empty() ? 0 : std::stoi(row[3]);
    item.added_at = row[4];
    item.title = row[5];
    item.artist = row[6];
    item.file_hash = row[7];
    items.push_back(std::move(item));
  }
  return items;
}

bool DatabasePool::add_playlist_item(int64_t playlist_id, int64_t music_id) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto max_result = conn->query("SELECT COALESCE(MAX(sort_order), -1) + 1 "
                                "FROM playlist_items WHERE playlist_id = ?",
                                {std::to_string(playlist_id)});
  int next_order = 0;
  if (max_result && !max_result->rows.empty() && !max_result->rows[0].empty()) {
    next_order = std::stoi(max_result->rows[0][0]);
  }

  auto affected = conn->execute("INSERT IGNORE INTO playlist_items "
                                "(playlist_id, music_id, sort_order) VALUES (?, ?, ?)",
                                {std::to_string(playlist_id), std::to_string(music_id), std::to_string(next_order)});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

bool DatabasePool::remove_playlist_item(int64_t playlist_id, int64_t music_id) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected = conn->execute("DELETE FROM playlist_items "
                                "WHERE playlist_id = ? AND music_id = ?",
                                {std::to_string(playlist_id), std::to_string(music_id)});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

bool DatabasePool::reorder_playlist_items(int64_t playlist_id, const std::vector<int64_t>& music_ids) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected = conn->execute("DELETE FROM playlist_items WHERE playlist_id = ?", {std::to_string(playlist_id)});
  if (!affected.has_value()) {
    release_connection(std::move(conn));
    return false;
  }

  bool ok = true;
  for (std::size_t i = 0; i < music_ids.size(); ++i) {
    affected =
      conn->execute("INSERT INTO playlist_items (playlist_id, music_id, sort_order) "
                    "VALUES (?, ?, ?)",
                    {std::to_string(playlist_id), std::to_string(music_ids[i]), std::to_string(static_cast<int>(i))});
    if (!affected.has_value()) {
      ok = false;
      break;
    }
  }

  release_connection(std::move(conn));
  return ok;
}

} // namespace hps
