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

  auto result = conn->query("SELECT user_id, username, password_hash, role, email, created_at "
                            "FROM users WHERE user_id = ?",
                            {std::to_string(user_id)});

  release_connection(std::move(conn));

  if (!result || result->rows.empty()) {
    return std::nullopt;
  }

  User u{};
  const auto& row = result->rows[0];
  if (row.size() >= 6) {
    u.user_id = row[0].empty() ? 0 : std::stoll(row[0]);
    u.username = row[1];
    u.password_hash = row[2];
    u.role = to_user_role(row[3].empty() ? 0 : std::stoi(row[3]));
    u.email = row[4];
    u.created_at = row[5];
  }
  return u;
}

bool DatabasePool::create_user(const User& user) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected =
    conn->execute("INSERT INTO users (username, password_hash, role, email) VALUES (?, ?, ?, ?)",
                  {user.username, user.password_hash, std::to_string(to_role_int(user.role)), user.email});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

bool DatabasePool::store_file_record(const FileRecord& record) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected = conn->execute("INSERT INTO files (file_name, file_hash, file_size, content_type, chunk_size) "
                                "VALUES (?, ?, ?, ?, ?) "
                                "ON DUPLICATE KEY UPDATE file_name=VALUES(file_name), "
                                "file_size=VALUES(file_size), content_type=VALUES(content_type)",
                                {record.file_name,
                                 record.file_hash,
                                 std::to_string(record.file_size),
                                 record.content_type,
                                 std::to_string(record.chunk_size)});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

std::optional<FileRecord> DatabasePool::get_file_record(int64_t file_id) {
  auto conn = get_connection();
  if (!conn) {
    return std::nullopt;
  }

  auto result = conn->query("SELECT file_id, file_name, file_hash, file_size, content_type, chunk_size, created_at "
                            "FROM files WHERE file_id = ?",
                            {std::to_string(file_id)});

  release_connection(std::move(conn));

  if (!result || result->rows.empty()) {
    return std::nullopt;
  }

  const auto& row = result->rows[0];
  if (row.size() < 7) {
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
  return r;
}

std::optional<FileRecord> DatabasePool::get_file_record_by_hash(const std::string& hash) {
  auto conn = get_connection();
  if (!conn) {
    return std::nullopt;
  }

  auto result = conn->query("SELECT file_id, file_name, file_hash, file_size, content_type, chunk_size, created_at "
                            "FROM files WHERE file_hash = ?",
                            {hash});

  release_connection(std::move(conn));

  if (!result || result->rows.empty()) {
    return std::nullopt;
  }

  const auto& row = result->rows[0];
  if (row.size() < 7) {
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
  return r;
}

std::vector<FileRecord> DatabasePool::search_files(const std::string& name_pattern, int offset, int limit) {
  auto conn = get_connection();
  if (!conn) {
    return {};
  }

  std::string sql = "SELECT file_id, file_name, file_hash, file_size, content_type, chunk_size, created_at "
                    "FROM files";
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
    if (row.size() < 7) {
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

} // namespace hps
