#include "database_pool.h"

#include "boost_mysql_connection.h"

#include <sstream>
#include <thread>
#include <utility>

namespace hps {

namespace {

std::unique_ptr<IConnection> default_factory() {
  return std::make_unique<BoostMySqlConnection>();
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
      // 清理已建立的连接
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

  // 等待所有活跃连接归还
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

  // 健康检查
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
    // closed_ 时 conn 随作用域结束自动 close
  }
  cv_.notify_one();
}

// ---- 业务方法 ----

namespace {

// 简易 SQL 参数占位符替换，用于构建 prepared statement 语句
// 将 ? 替换为实际值，仅用于非 prepared statement 时的回退
// 业务层统一使用 prepared statement，此函数仅作为内部辅助
[[maybe_unused]] std::string escape_string(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '\'';
  for (char c : s) {
    if (c == '\'') {
      out += "''";
    } else if (c == '\\') {
      out += "\\\\";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
}

} // namespace

std::optional<User> DatabasePool::get_user(int64_t user_id) {
  auto conn = get_connection();
  if (!conn) {
    return std::nullopt;
  }

  auto result = conn->query("SELECT user_id, username, password_hash, email, created_at "
                            "FROM users WHERE user_id = ?",
                            {std::to_string(user_id)});

  release_connection(std::move(conn));

  if (!result || result->rows.empty()) {
    return std::nullopt;
  }

  User u{};
  const auto& row = result->rows[0];
  if (row.size() >= 5) {
    u.user_id = row[0].empty() ? 0 : std::stoll(row[0]);
    u.username = row[1];
    u.password_hash = row[2];
    u.email = row[3];
    u.created_at = row[4];
  }
  return u;
}

bool DatabasePool::create_user(const User& user) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected = conn->execute("INSERT INTO users (username, password_hash, email) VALUES (?, ?, ?)",
                                {user.username, user.password_hash, user.email});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

bool DatabasePool::log_download(const DownloadLog& log) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected = conn->execute("INSERT INTO download_logs (user_id, file_hash) VALUES (?, ?)",
                                {std::to_string(log.user_id), log.file_hash});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

std::vector<DownloadLog> DatabasePool::get_download_history(int64_t user_id) {
  auto conn = get_connection();
  if (!conn) {
    return {};
  }

  auto result = conn->query("SELECT log_id, user_id, file_hash, downloaded_at "
                            "FROM download_logs WHERE user_id = ? "
                            "ORDER BY downloaded_at DESC",
                            {std::to_string(user_id)});

  release_connection(std::move(conn));

  std::vector<DownloadLog> logs;
  if (!result) {
    return logs;
  }
  logs.reserve(result->rows.size());
  for (const auto& row : result->rows) {
    if (row.size() < 4) {
      continue;
    }
    DownloadLog dl{};
    dl.log_id = row[0].empty() ? 0 : std::stoll(row[0]);
    dl.user_id = row[1].empty() ? 0 : std::stoll(row[1]);
    dl.file_hash = row[2];
    dl.downloaded_at = row[3];
    logs.push_back(std::move(dl));
  }
  return logs;
}

bool DatabasePool::store_file_meta(const FileMeta& meta) {
  auto conn = get_connection();
  if (!conn) {
    return false;
  }

  auto affected = conn->execute("INSERT INTO file_meta (file_hash, file_path, file_size, content_type) "
                                "VALUES (?, ?, ?, ?) "
                                "ON DUPLICATE KEY UPDATE file_path=VALUES(file_path), "
                                "file_size=VALUES(file_size), content_type=VALUES(content_type)",
                                {meta.file_hash, meta.file_path, std::to_string(meta.file_size), meta.content_type});

  release_connection(std::move(conn));
  return affected.has_value() && *affected > 0;
}

std::optional<FileMeta> DatabasePool::get_file_meta(const std::string& hash) {
  auto conn = get_connection();
  if (!conn) {
    return std::nullopt;
  }

  auto result = conn->query("SELECT file_hash, file_path, file_size, content_type, created_at "
                            "FROM file_meta WHERE file_hash = ?",
                            {hash});

  release_connection(std::move(conn));

  if (!result || result->rows.empty()) {
    return std::nullopt;
  }

  const auto& row = result->rows[0];
  if (row.size() < 5) {
    return std::nullopt;
  }

  FileMeta meta{};
  meta.file_hash = row[0];
  meta.file_path = row[1];
  meta.file_size = row[2].empty() ? 0 : std::stoull(row[2]);
  meta.content_type = row[3];
  meta.created_at = row[4];
  return meta;
}

} // namespace hps
