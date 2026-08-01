#include "database_pool.h"

#include "boost_mysql_connection.h"
#include "models.h"
#include "playlist_validation.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <random>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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

std::string random_claim_token() {
  std::array<unsigned char, 32> bytes{};
  std::random_device random;
  std::ranges::generate(bytes, [&random] { return static_cast<unsigned char>(random()); });
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string token;
  token.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    token.push_back(kHex[byte >> 4U]);
    token.push_back(kHex[byte & 0x0FU]);
  }
  return token;
}

} // namespace

DatabasePool::DatabasePool(ConnectionFactory factory) : factory_(factory ? std::move(factory) : default_factory) {}

DatabasePool::~DatabasePool() {
  do_close();
}

void DatabasePool::close() {
  do_close();
}

bool DatabasePool::with_connection(const std::function<bool(IConnection&)>& operation) {
  auto connection = get_connection();
  if (!connection) {
    return false;
  }
  const auto rollback_and_release = [this](std::unique_ptr<IConnection> connection_to_reset) {
    bool rollback_succeeded = false;
    try {
      rollback_succeeded = connection_to_reset->execute("ROLLBACK").has_value();
    } catch (...) {
      rollback_succeeded = false; // 吞掉 ROLLBACK 异常，降级关闭连接
    }
    if (!rollback_succeeded) {
      try {
        connection_to_reset->close();
      } catch (...) {
        rollback_succeeded = false;
      }
    }
    release_connection(std::move(connection_to_reset));
  };
  try {
    const bool result = operation(*connection);
    if (!result) {
      rollback_and_release(std::move(connection));
      return false;
    }
    release_connection(std::move(connection));
    return true;
  } catch (...) {
    rollback_and_release(std::move(connection));
    return false;
  }
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
  switch (v) {
    case 1:
      return UserRole::NORMAL;
    case 2:
      return UserRole::VIP;
    case 3:
      return UserRole::ADMIN;
    default:
      return UserRole::GUEST;
  }
}

template <typename Integer>
bool parse_integer(std::string_view text, Integer& value) {
  if (text.empty()) {
    return false;
  }
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

std::string escape_like_pattern(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    if (character == '\\' || character == '%' || character == '_') {
      escaped.push_back('\\');
    }
    escaped.push_back(character);
  }
  return escaped;
}

LookupResult<User> parse_user_result(const std::optional<QueryResult>& result, bool allow_admin_vip_expiry) {
  if (!result) {
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }
  if (result->rows.empty()) {
    return {LookupStatus::NOT_FOUND, std::nullopt};
  }
  const auto& row = result->rows[0];
  if (row.size() != 8) {
    return {LookupStatus::INVALID_DATA, std::nullopt};
  }
  User user;
  int role = 0;
  if (!parse_integer(row[0], user.user_id) || user.user_id <= 0 || !parse_integer(row[4], role) || role < 0 ||
      role > 3) {
    return {LookupStatus::INVALID_DATA, std::nullopt};
  }
  user.username = row[1];
  user.password_hash = row[2];
  user.salt = row[3];
  user.role = to_user_role(role);
  user.email = row[5];
  if (!row[6].empty()) {
    user.vip_expires_at = parse_mysql_utc_datetime(row[6]);
    if (!user.vip_expires_at) {
      return {LookupStatus::INVALID_DATA, std::nullopt};
    }
  }
  const bool is_repairable_admin_vip_expiry = allow_admin_vip_expiry && user.role == UserRole::ADMIN &&
                                              user.vip_expires_at.has_value();
  if (!has_valid_vip_expiry_state(user) && !is_repairable_admin_vip_expiry) {
    return {LookupStatus::INVALID_DATA, std::nullopt};
  }
  user.created_at = row[7];
  return {LookupStatus::FOUND, std::move(user)};
}

LookupResult<FileRecord> parse_file_record_result(const std::optional<QueryResult>& result) {
  if (!result) {
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }
  if (result->rows.empty()) {
    return {LookupStatus::NOT_FOUND, std::nullopt};
  }
  if (result->rows.size() != 1 || result->rows[0].size() != 9) {
    return {LookupStatus::INVALID_DATA, std::nullopt};
  }
  const auto& row = result->rows[0];
  FileRecord record;
  if (row[1].empty() || row[2].empty() || row[4].empty() || row[6].empty() || !parse_integer(row[0], record.file_id) ||
      record.file_id <= 0 || !parse_integer(row[3], record.file_size) || !parse_integer(row[5], record.chunk_size) ||
      record.chunk_size <= 0 || !parse_integer(row[8], record.uploaded_by) || record.uploaded_by < 0 ||
      (!row[7].empty() && (!parse_integer(row[7], record.music_id) || record.music_id < 0)) ||
      !parse_mysql_utc_datetime(row[6])) {
    return {LookupStatus::INVALID_DATA, std::nullopt};
  }
  record.file_name = row[1];
  record.file_hash = row[2];
  record.content_type = row[4];
  record.created_at = row[6];
  return {LookupStatus::FOUND, std::move(record)};
}

struct PlaylistOwnerRequest {
  int64_t playlist_id{0};
  int64_t actor_id{0};
};

MutationStatus lock_playlist_owner(IConnection& connection, const PlaylistOwnerRequest& request) {
  const auto owner = connection.query("SELECT user_id FROM user_playlists WHERE playlist_id = ? FOR UPDATE",
                                      {std::to_string(request.playlist_id)});
  if (!owner) {
    return MutationStatus::STORAGE_ERROR;
  }
  if (owner->rows.empty()) {
    return MutationStatus::NOT_FOUND;
  }
  int64_t owner_id = 0;
  if (owner->rows.size() != 1 || owner->rows[0].size() != 1 || !parse_integer(owner->rows[0][0], owner_id) ||
      owner_id <= 0) {
    return MutationStatus::INVALID_STATE;
  }
  return owner_id == request.actor_id ? MutationStatus::OK : MutationStatus::OWNER_REQUIRED;
}

struct LockedPlaylistItem {
  int64_t music_id{0};
  int sort_order{0};
};

MutationResult<std::vector<LockedPlaylistItem>> lock_playlist_items(IConnection& connection, int64_t playlist_id) {
  const auto query = connection.query("SELECT music_id, sort_order FROM playlist_items "
                                      "WHERE playlist_id = ? ORDER BY sort_order FOR UPDATE",
                                      {std::to_string(playlist_id)});
  if (!query)
    return {MutationStatus::STORAGE_ERROR, std::nullopt, "PERSISTENCE_ERROR"};
  std::unordered_set<int64_t> music_ids;
  std::unordered_set<int> positions;
  std::vector<LockedPlaylistItem> items;
  items.reserve(query->rows.size());
  for (const auto& row : query->rows) {
    LockedPlaylistItem item;
    if (row.size() != 2 || !parse_integer(row[0], item.music_id) || !parse_integer(row[1], item.sort_order) ||
        item.music_id <= 0 || item.sort_order < 0 || !music_ids.insert(item.music_id).second ||
        !positions.insert(item.sort_order).second) {
      return {MutationStatus::INVALID_STATE, std::nullopt, "INVALID_REQUEST"};
    }
    items.push_back(item);
  }
  return {MutationStatus::OK, std::move(items), std::nullopt};
}

MutationResult<Playlist> parse_playlist_result(const std::optional<QueryResult>& query,
                                               int64_t expected_playlist_id,
                                               int64_t expected_user_id) {
  if (!query)
    return {MutationStatus::STORAGE_ERROR, std::nullopt, "PERSISTENCE_ERROR"};
  if (query->rows.empty())
    return {MutationStatus::NOT_FOUND, std::nullopt, "PLAYLIST_NOT_FOUND"};
  if (query->rows.size() != 1 || query->rows[0].size() != 6)
    return {MutationStatus::INVALID_STATE, std::nullopt, "INVALID_REQUEST"};
  const auto& row = query->rows[0];
  Playlist playlist;
  if (!parse_integer(row[0], playlist.playlist_id) || !parse_integer(row[1], playlist.user_id) ||
      !parse_integer(row[4], playlist.item_count) || playlist.playlist_id != expected_playlist_id ||
      playlist.user_id != expected_user_id || playlist.item_count < 0 || !is_valid_playlist_text(row[2], row[3])) {
    return {MutationStatus::INVALID_STATE, std::nullopt, "INVALID_REQUEST"};
  }
  playlist.name = row[2];
  playlist.description = row[3];
  playlist.created_at = row[5];
  return {MutationStatus::OK, std::move(playlist), std::nullopt};
}

MutationResult<Playlist> read_playlist(IConnection& connection, int64_t playlist_id, int64_t user_id) {
  return parse_playlist_result(
    connection.query("SELECT p.playlist_id, p.user_id, p.name, p.description, "
                     "(SELECT COUNT(*) FROM playlist_items pi WHERE pi.playlist_id = p.playlist_id), p.created_at "
                     "FROM user_playlists p WHERE p.playlist_id = ?",
                     {std::to_string(playlist_id)}),
    playlist_id,
    user_id);
}

struct FileDeletionRequest {
  int64_t file_id{0};
  int64_t actor_id{0};
  bool can_delete_any{false};
};

struct FileDeletionState {
  int64_t preview_music_id{0};
  int64_t music_id{0};
  std::string file_hash;
  std::vector<std::string> chunk_hashes;
  std::vector<std::string> unshared_chunk_hashes;
};

struct ChunkReferenceRequest {
  std::string_view file_hash;
  std::string_view chunk_hash;
};

using FileDeletionResult = MutationResult<FileDeletionPlan>;

bool read_file_deletion_preview(IConnection& connection,
                                const FileDeletionRequest& request,
                                FileDeletionState& state,
                                FileDeletionResult& result) {
  const auto preview =
    connection.query("SELECT music_id FROM file_records WHERE file_id = ?", {std::to_string(request.file_id)});
  if (!preview) {
    return false;
  }
  if (preview->rows.empty()) {
    result = {MutationStatus::NOT_FOUND, std::nullopt, "FILE_NOT_FOUND"};
    return false;
  }
  if (preview->rows.size() != 1 || preview->rows[0].size() != 1) {
    result = {MutationStatus::INVALID_STATE, std::nullopt, "FILE_STATE_INVALID"};
    return false;
  }
  state.preview_music_id = 0;
  if (!preview->rows[0][0].empty() &&
      (!parse_integer(preview->rows[0][0], state.preview_music_id) || state.preview_music_id <= 0)) {
    result = {MutationStatus::INVALID_STATE, std::nullopt, "FILE_STATE_INVALID"};
    return false;
  }
  return true;
}

bool lock_preview_music(IConnection& connection, const FileDeletionState& state, FileDeletionResult& result) {
  if (state.preview_music_id <= 0) {
    return true;
  }
  const auto music = connection.query("SELECT music_id FROM music_meta WHERE music_id = ? FOR UPDATE",
                                      {std::to_string(state.preview_music_id)});
  int64_t locked_music_id = 0;
  if (!music || music->rows.size() != 1 || music->rows[0].size() != 1 ||
      !parse_integer(music->rows[0][0], locked_music_id) || locked_music_id != state.preview_music_id) {
    result = {MutationStatus::INVALID_STATE, std::nullopt, "FILE_MUSIC_STATE_INVALID"};
    return false;
  }
  return true;
}

bool lock_file_for_deletion(IConnection& connection,
                            const FileDeletionRequest& request,
                            FileDeletionState& state,
                            FileDeletionResult& result) {
  const auto file = connection.query(
    "SELECT file_id, file_name, file_hash, file_size, content_type, chunk_size, created_at, music_id, uploaded_by "
    "FROM file_records WHERE file_id = ? FOR UPDATE",
    {std::to_string(request.file_id)});
  if (!file) {
    return false;
  }
  if (file->rows.empty()) {
    result = {MutationStatus::NOT_FOUND, std::nullopt, "FILE_NOT_FOUND"};
    return false;
  }
  if (file->rows[0].size() < 9) {
    result = {MutationStatus::INVALID_STATE, std::nullopt, "FILE_STATE_INVALID"};
    return false;
  }
  const auto& row = file->rows[0];
  int64_t owner = 0;
  int64_t music_id = 0;
  if (!parse_integer(row[8], owner) || (!row[7].empty() && !parse_integer(row[7], music_id))) {
    result = {MutationStatus::INVALID_STATE, std::nullopt, "FILE_STATE_INVALID"};
    return false;
  }
  if (music_id != state.preview_music_id) {
    result = {MutationStatus::CONFLICT, std::nullopt, "FILE_MUSIC_CHANGED"};
    return false;
  }
  if (!request.can_delete_any && owner != request.actor_id) {
    result = {MutationStatus::OWNER_REQUIRED, std::nullopt, "FILE_DELETE_FORBIDDEN"};
    return false;
  }
  state.music_id = music_id;
  state.file_hash = row[2];
  return true;
}

bool read_file_chunk_hashes(IConnection& connection, FileDeletionState& state, FileDeletionResult& result) {
  const auto chunks =
    connection.query("SELECT chunk_hash FROM file_chunks WHERE file_hash = ? ORDER BY chunk_index", {state.file_hash});
  if (!chunks) {
    return false;
  }
  state.chunk_hashes.clear();
  state.chunk_hashes.reserve(chunks->rows.size());
  for (const auto& chunk : chunks->rows) {
    if (chunk.size() != 1 || chunk[0].empty()) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "FILE_CHUNK_STATE_INVALID"};
      return false;
    }
    state.chunk_hashes.push_back(chunk[0]);
  }
  std::ranges::sort(state.chunk_hashes);
  const auto unique_end = std::unique(state.chunk_hashes.begin(), state.chunk_hashes.end());
  state.chunk_hashes.erase(unique_end, state.chunk_hashes.end());
  return true;
}

bool check_chunk_is_unshared(IConnection& connection,
                             const ChunkReferenceRequest& request,
                             bool& is_unshared,
                             FileDeletionResult& result) {
  const auto references = connection.query(
    "SELECT file_hash FROM file_chunks WHERE chunk_hash = ? ORDER BY file_hash, chunk_index FOR UPDATE",
    {std::string(request.chunk_hash)});
  if (!references) {
    return false;
  }
  bool has_other_file = false;
  for (const auto& reference : references->rows) {
    if (reference.size() != 1 || reference[0].empty()) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "FILE_CHUNK_STATE_INVALID"};
      return false;
    }
    if (reference[0] != request.file_hash) {
      has_other_file = true;
    }
  }
  is_unshared = !has_other_file;
  return true;
}

bool find_unshared_file_chunks(IConnection& connection, FileDeletionState& state, FileDeletionResult& result) {
  state.unshared_chunk_hashes.clear();
  for (const auto& hash : state.chunk_hashes) {
    bool is_unshared = false;
    if (!check_chunk_is_unshared(connection, ChunkReferenceRequest{state.file_hash, hash}, is_unshared, result)) {
      return false;
    }
    if (is_unshared) {
      state.unshared_chunk_hashes.push_back(hash);
    }
  }
  return true;
}

bool delete_file_and_queue_chunks(IConnection& connection,
                                  const FileDeletionRequest& request,
                                  const FileDeletionState& state) {
  const auto deleted =
    connection.execute("DELETE FROM file_records WHERE file_id = ?", {std::to_string(request.file_id)});
  if (!deleted || *deleted != 1) {
    return false;
  }
  for (const auto& hash : state.unshared_chunk_hashes) {
    const auto queued = connection.execute(
      "INSERT INTO pending_chunk_deletions (chunk_hash, state, claim_token, claimed_at, next_attempt_at) "
      "VALUES (?, 'PENDING', NULL, NULL, UTC_TIMESTAMP(6)) ON DUPLICATE KEY UPDATE "
      "state = 'PENDING', claim_token = NULL, claimed_at = NULL, next_attempt_at = UTC_TIMESTAMP(6)",
      {hash});
    if (!queued) {
      return false;
    }
  }
  return true;
}

bool delete_unused_music_meta(IConnection& connection, const FileDeletionState& state) {
  if (state.music_id <= 0) {
    return true;
  }
  const auto remaining = connection.query("SELECT COUNT(*) FROM file_records WHERE music_id = ? FOR UPDATE",
                                          {std::to_string(state.music_id)});
  int count = 0;
  if (!remaining || remaining->rows.size() != 1 || remaining->rows[0].size() != 1 ||
      !parse_integer(remaining->rows[0][0], count)) {
    return false;
  }
  if (count != 0) {
    return true;
  }
  const auto music_deleted =
    connection.execute("DELETE FROM music_meta WHERE music_id = ?", {std::to_string(state.music_id)});
  return music_deleted.has_value();
}

} // namespace

LookupResult<User> DatabasePool::lookup_user(const std::string& sql,
                                             const std::vector<std::string>& params,
                                             bool allow_admin_vip_expiry) {
  std::unique_ptr<IConnection> conn;
  try {
    conn = get_connection();
  } catch (...) {
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }
  if (!conn) {
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }

  std::optional<QueryResult> result;
  try {
    result = conn->query(sql, params);
  } catch (...) {
    release_connection(std::move(conn));
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }

  release_connection(std::move(conn));

  return parse_user_result(result, allow_admin_vip_expiry);
}

LookupResult<User> DatabasePool::get_user_result(int64_t user_id) {
  return lookup_user("SELECT user_id, username, password_hash, salt, role, email, vip_expires_at, created_at "
                     "FROM users WHERE user_id = ?",
                     {std::to_string(user_id)},
                     false);
}

MutationResult<std::monostate> DatabasePool::create_user(const User& user) {
  if (!user.email.empty() && user.email.size() > kMaximumEmailLength) {
    return {MutationStatus::INVALID_STATE, std::nullopt, "EMAIL_INVALID"};
  }
  std::unique_ptr<IConnection> connection;
  std::optional<int64_t> affected;
  try {
    connection = get_connection();
    if (connection) {
      affected = connection->execute(
        "INSERT INTO users (username, password_hash, salt, role, email) VALUES (?, ?, ?, ?, NULLIF(?, ''))",
        {user.username, user.password_hash, user.salt, std::to_string(to_role_int(user.role)), user.email});
    }
  } catch (...) {
    affected = std::nullopt; // 连接异常，后续通过查询检测冲突
  }
  release_connection(std::move(connection));
  if (affected && *affected > 0) {
    return {MutationStatus::OK, std::monostate{}, std::nullopt};
  }
  if (get_user_by_username_result(user.username).status == LookupStatus::FOUND) {
    return {MutationStatus::CONFLICT, std::nullopt, "USERNAME_CONFLICT"};
  }
  if (!user.email.empty() && get_user_by_email_result(user.email).status == LookupStatus::FOUND) {
    return {MutationStatus::CONFLICT, std::nullopt, "EMAIL_CONFLICT"};
  }
  return {MutationStatus::STORAGE_ERROR, std::nullopt, "USER_CREATE_FAILED"};
}

LookupResult<User> DatabasePool::get_admin_user_result() {
  return lookup_user("SELECT user_id, username, password_hash, salt, role, email, vip_expires_at, created_at "
                     "FROM users WHERE role = 3 LIMIT 1",
                     {},
                     true);
}

LookupResult<User> DatabasePool::get_user_by_username_result(const std::string& username) {
  return lookup_user("SELECT user_id, username, password_hash, salt, role, email, vip_expires_at, created_at "
                     "FROM users WHERE username = ? LIMIT 1",
                     {username},
                     false);
}

LookupResult<User> DatabasePool::get_user_by_email_result(const std::string& email) {
  return lookup_user("SELECT user_id, username, password_hash, salt, role, email, vip_expires_at, created_at "
                     "FROM users WHERE email = ? LIMIT 1",
                     {email},
                     false);
}

MutationResult<std::monostate> DatabasePool::create_admin_user(const User& user) {
  if (user.email.empty() || user.email.size() > kMaximumEmailLength) {
    return {MutationStatus::INVALID_STATE, std::nullopt, "EMAIL_INVALID"};
  }
  std::unique_ptr<IConnection> connection;
  std::optional<int64_t> affected;
  try {
    connection = get_connection();
    if (connection) {
      affected = connection->execute("INSERT INTO users (username, password_hash, salt, role, email, vip_expires_at) "
                                     "VALUES (?, ?, ?, 3, NULLIF(?, ''), NULL)",
                                     {user.username, user.password_hash, user.salt, user.email});
    }
  } catch (...) {
    affected = std::nullopt; // 管理员创建异常，后续检测冲突
  }
  release_connection(std::move(connection));
  if (affected && *affected > 0) {
    return {MutationStatus::OK, std::monostate{}, std::nullopt};
  }

  const auto admin = get_admin_user_result();
  if (admin.status == LookupStatus::FOUND) {
    return {MutationStatus::CONFLICT, std::nullopt, "ADMIN_SLOT_CONFLICT"};
  }
  const auto username = get_user_by_username_result(user.username);
  if (username.status == LookupStatus::FOUND) {
    return {MutationStatus::CONFLICT, std::nullopt, "ADMIN_USERNAME_CONFLICT"};
  }
  const auto email = get_user_by_email_result(user.email);
  if (email.status == LookupStatus::FOUND) {
    return {MutationStatus::CONFLICT, std::nullopt, "ADMIN_EMAIL_CONFLICT"};
  }
  return {MutationStatus::STORAGE_ERROR, std::nullopt, "ADMIN_CREATE_FAILED"};
}

MutationResult<std::monostate> DatabasePool::update_admin_credentials(const User& user) {
  if (user.email.empty() || user.email.size() > kMaximumEmailLength) {
    return {MutationStatus::INVALID_STATE, std::nullopt, "EMAIL_INVALID"};
  }
  std::unique_ptr<IConnection> connection;
  std::optional<int64_t> affected;
  try {
    connection = get_connection();
    if (connection) {
      affected = connection->execute("UPDATE users SET vip_expires_at = NULL, email = NULLIF(?, ''), "
                                     "password_hash = ?, salt = ? WHERE user_id = ? AND role = 3",
                                     {user.email, user.password_hash, user.salt, std::to_string(user.user_id)});
    }
  } catch (...) {
    affected = std::nullopt; // 管理员凭证更新异常，后续检测状态
  }
  release_connection(std::move(connection));
  if (affected && *affected > 0) {
    return {MutationStatus::OK, std::monostate{}, std::nullopt};
  }
  if (affected && *affected == 0) {
    return {MutationStatus::NOT_FOUND, std::nullopt, "ADMIN_UPDATE_TARGET_MISSING"};
  }
  const auto email = get_user_by_email_result(user.email);
  if (email.status == LookupStatus::FOUND && email.value && email.value->user_id != user.user_id) {
    return {MutationStatus::CONFLICT, std::nullopt, "ADMIN_EMAIL_CONFLICT"};
  }
  return {MutationStatus::STORAGE_ERROR, std::nullopt, "ADMIN_UPDATE_FAILED"};
}

// IDatabasePool 与路由合同固定 user_id、duration_days 的公共参数顺序，不能改为强类型接口。
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters,readability-function-cognitive-complexity)
MutationResult<User> DatabasePool::grant_or_extend_vip(int64_t user_id,
                                                       int duration_days,
                                                       std::chrono::system_clock::time_point now) {
  if (duration_days != 30 && duration_days != 90 && duration_days != 365) {
    return {MutationStatus::INVALID_STATE, std::nullopt, "INVALID_VIP_DURATION"};
  }

  MutationResult<User> result{MutationStatus::STORAGE_ERROR, std::nullopt, "VIP_MUTATION_FAILED"};
  const bool committed = with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION")) {
      return false;
    }
    const auto locked = parse_user_result(
      connection.query("SELECT user_id, username, password_hash, salt, role, email, vip_expires_at, created_at "
                       "FROM users WHERE user_id = ? FOR UPDATE",
                       {std::to_string(user_id)}),
      false);
    if (locked.status == LookupStatus::NOT_FOUND) {
      result = {MutationStatus::NOT_FOUND, std::nullopt, "USER_NOT_FOUND"};
      return false;
    }
    if (locked.status == LookupStatus::INVALID_DATA) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "VIP_STATE_INVALID"};
      return false;
    }
    if (locked.status != LookupStatus::FOUND || !locked.value) {
      return false;
    }
    if (!has_valid_vip_expiry_state(*locked.value)) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "VIP_STATE_INVALID"};
      return false;
    }
    if (locked.value->role == UserRole::ADMIN) {
      result = {MutationStatus::CONFLICT, std::nullopt, "ADMIN_MEMBERSHIP_FORBIDDEN"};
      return false;
    }
    if (locked.value->role != UserRole::NORMAL && locked.value->role != UserRole::VIP) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "VIP_STATE_INVALID"};
      return false;
    }

    auto user = *locked.value;
    const auto base = user.vip_expires_at && *user.vip_expires_at > now ? *user.vip_expires_at : now;
    const auto extension =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::hours{24 * duration_days});
    if (base > std::chrono::system_clock::time_point::max() - extension) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "VIP_EXPIRY_OUT_OF_RANGE"};
      return false;
    }
    const auto expires_at = base + extension;
    const auto mysql_expires_at = try_format_mysql_utc_datetime(expires_at);
    if (!mysql_expires_at) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "VIP_EXPIRY_OUT_OF_RANGE"};
      return false;
    }
    const auto affected = connection.execute("UPDATE users SET role = 2, vip_expires_at = ? WHERE user_id = ?",
                                             {*mysql_expires_at, std::to_string(user_id)});
    if (!affected || *affected != 1) {
      return false;
    }
    if (!connection.execute("COMMIT")) {
      return false;
    }
    user.role = UserRole::VIP;
    user.vip_expires_at = expires_at;
    result = {MutationStatus::OK, std::move(user), std::nullopt};
    return true;
  });
  if (!committed && result.status == MutationStatus::OK) {
    return {MutationStatus::STORAGE_ERROR, std::nullopt, "VIP_COMMIT_FAILED"};
  }
  return result;
}

MutationResult<User> DatabasePool::revoke_vip(int64_t user_id) {
  MutationResult<User> result{MutationStatus::STORAGE_ERROR, std::nullopt, "VIP_REVOKE_FAILED"};
  const bool committed = with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION")) {
      return false;
    }
    const auto locked = parse_user_result(
      connection.query("SELECT user_id, username, password_hash, salt, role, email, vip_expires_at, created_at "
                       "FROM users WHERE user_id = ? FOR UPDATE",
                       {std::to_string(user_id)}),
      false);
    if (locked.status == LookupStatus::NOT_FOUND) {
      result = {MutationStatus::NOT_FOUND, std::nullopt, "USER_NOT_FOUND"};
      return false;
    }
    if (locked.status == LookupStatus::INVALID_DATA) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "VIP_STATE_INVALID"};
      return false;
    }
    if (locked.status != LookupStatus::FOUND || !locked.value) {
      return false;
    }
    if (!has_valid_vip_expiry_state(*locked.value)) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "VIP_STATE_INVALID"};
      return false;
    }
    if (locked.value->role == UserRole::ADMIN) {
      result = {MutationStatus::CONFLICT, std::nullopt, "ADMIN_MEMBERSHIP_FORBIDDEN"};
      return false;
    }
    if (locked.value->role != UserRole::NORMAL && locked.value->role != UserRole::VIP) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "VIP_STATE_INVALID"};
      return false;
    }

    auto user = *locked.value;
    const auto affected = connection.execute("UPDATE users SET role = 1, vip_expires_at = NULL WHERE user_id = ?",
                                             {std::to_string(user_id)});
    if (!affected || *affected != 1) {
      return false;
    }
    if (!connection.execute("COMMIT")) {
      return false;
    }
    user.role = UserRole::NORMAL;
    user.vip_expires_at.reset();
    result = {MutationStatus::OK, std::move(user), std::nullopt};
    return true;
  });
  if (!committed && result.status == MutationStatus::OK) {
    return {MutationStatus::STORAGE_ERROR, std::nullopt, "VIP_COMMIT_FAILED"};
  }
  return result;
}

LookupResult<AdminUserPage> DatabasePool::list_admin_users(const std::string& query, int offset, int limit) {
  LookupResult<AdminUserPage> result{LookupStatus::STORAGE_ERROR, std::nullopt};
  const bool committed = with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION WITH CONSISTENT SNAPSHOT")) {
      return false;
    }
    const bool filtered = !query.empty();
    const std::string where = filtered ? R"( WHERE username LIKE ? ESCAPE '\\' OR email LIKE ? ESCAPE '\\')" : "";
    const std::string pattern = "%" + escape_like_pattern(query) + "%";
    std::vector<std::string> filter_params;
    if (filtered) {
      filter_params = {pattern, pattern};
    }
    const auto count_result = connection.query("SELECT COUNT(*) FROM users" + where, filter_params);
    if (!count_result || count_result->rows.size() != 1 || count_result->rows[0].size() != 1) {
      return false;
    }
    int total = 0;
    if (!parse_integer(count_result->rows[0][0], total) || total < 0) {
      result = {LookupStatus::INVALID_DATA, std::nullopt};
      return false;
    }

    auto item_params = filter_params;
    item_params.push_back(std::to_string(limit));
    item_params.push_back(std::to_string(offset));
    const auto item_result = connection.query(
      "SELECT user_id, username, password_hash, salt, role, email, vip_expires_at, created_at FROM users" + where +
        " ORDER BY user_id ASC LIMIT ? OFFSET ?",
      item_params);
    if (!item_result) {
      return false;
    }

    AdminUserPage page;
    page.total = total;
    page.offset = offset;
    page.limit = limit;
    page.items.reserve(item_result->rows.size());
    for (const auto& row : item_result->rows) {
      QueryResult single_row{.rows = {row}};
      auto parsed = parse_user_result(single_row, false);
      if (parsed.status != LookupStatus::FOUND || !parsed.value) {
        result = {LookupStatus::INVALID_DATA, std::nullopt};
        return false;
      }
      page.items.push_back(std::move(*parsed.value));
    }
    if (!connection.execute("COMMIT")) {
      return false;
    }
    result = {LookupStatus::FOUND, std::move(page)};
    return true;
  });
  if (!committed) {
    if (result.status == LookupStatus::FOUND) {
      return {LookupStatus::STORAGE_ERROR, std::nullopt};
    }
    return result;
  }
  return result;
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
  return get_file_record_result(file_id).value;
}

LookupResult<FileRecord> DatabasePool::get_file_record_result(int64_t file_id) {
  std::unique_ptr<IConnection> connection;
  try {
    connection = get_connection();
    if (!connection) {
      return {LookupStatus::STORAGE_ERROR, std::nullopt};
    }
    const auto result = connection->query("SELECT file_id, file_name, file_hash, file_size, content_type, "
                                          "chunk_size, created_at, music_id, uploaded_by "
                                          "FROM file_records WHERE file_id = ?",
                                          {std::to_string(file_id)});
    release_connection(std::move(connection));
    return parse_file_record_result(result);
  } catch (...) {
    release_connection(std::move(connection));
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }
}

std::optional<FileRecord> DatabasePool::get_file_record_by_hash(const std::string& hash) {
  std::unique_ptr<IConnection> connection;
  try {
    connection = get_connection();
    if (!connection) {
      return std::nullopt;
    }
    const auto result = connection->query("SELECT file_id, file_name, file_hash, file_size, content_type, "
                                          "chunk_size, created_at, music_id, uploaded_by "
                                          "FROM file_records WHERE file_hash = ?",
                                          {hash});
    release_connection(std::move(connection));
    return parse_file_record_result(result).value;
  } catch (...) {
    release_connection(std::move(connection));
    return std::nullopt;
  }
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

LookupResult<AuthUser> DatabasePool::get_auth_user_result(const std::string& username) {
  std::unique_ptr<IConnection> conn;
  try {
    conn = get_connection();
  } catch (...) {
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }
  if (!conn) {
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }

  std::optional<QueryResult> result;
  try {
    result = conn->query("SELECT user_id, username, role FROM users WHERE username = ?", {username});
  } catch (...) {
    release_connection(std::move(conn));
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }

  release_connection(std::move(conn));

  if (!result) {
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }
  if (result->rows.empty()) {
    return {LookupStatus::NOT_FOUND, std::nullopt};
  }

  const auto& row = result->rows[0];
  if (row.size() != 3) {
    return {LookupStatus::INVALID_DATA, std::nullopt};
  }

  AuthUser u{};
  int role = 0;
  if (!parse_integer(row[0], u.user_id) || u.user_id <= 0 || !parse_integer(row[2], role) || role < 0 || role > 3) {
    return {LookupStatus::INVALID_DATA, std::nullopt};
  }
  u.username = row[1];
  u.role = to_user_role(role);
  return {LookupStatus::FOUND, std::move(u)};
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
MutationResult<std::monostate> DatabasePool::update_user(const User& user) {
  const bool update_email = !user.email.empty();
  const bool update_password = !user.password_hash.empty() || !user.salt.empty();
  if (user.user_id <= 0 || (!update_email && !update_password) || user.password_hash.empty() != user.salt.empty()) {
    return {MutationStatus::INVALID_STATE, std::nullopt, "USER_UPDATE_INVALID"};
  }
  if (update_email && user.email.size() > kMaximumEmailLength) {
    return {MutationStatus::INVALID_STATE, std::nullopt, "EMAIL_INVALID"};
  }

  std::string sql{"UPDATE users SET "};
  std::vector<std::string> params;
  if (update_email) {
    sql += "email = ?";
    params.push_back(user.email);
  }
  if (update_password) {
    if (update_email) {
      sql += ", ";
    }
    sql += "password_hash = ?, salt = ?";
    params.push_back(user.password_hash);
    params.push_back(user.salt);
  }
  sql += " WHERE user_id = ?";
  params.push_back(std::to_string(user.user_id));

  bool update_failed = false;
  MutationResult<std::monostate> result{MutationStatus::STORAGE_ERROR, std::nullopt, "USER_UPDATE_FAILED"};
  const bool committed = with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION")) {
      return false;
    }
    const auto affected = connection.execute(sql, params);
    if (!affected) {
      update_failed = true;
      return false;
    }
    if (*affected == 0) {
      const auto stored = parse_user_result(
        connection.query("SELECT user_id, username, password_hash, salt, role, email, vip_expires_at, created_at "
                         "FROM users WHERE user_id = ?",
                         {std::to_string(user.user_id)}),
        false);
      if (stored.status == LookupStatus::NOT_FOUND) {
        result = {MutationStatus::NOT_FOUND, std::nullopt, "USER_NOT_FOUND"};
        return false;
      }
      if (stored.status != LookupStatus::FOUND || !stored.value) {
        return false;
      }
    }
    if (!connection.execute("COMMIT")) {
      return false;
    }
    result = {MutationStatus::OK, std::monostate{}, std::nullopt};
    return true;
  });
  if (committed) {
    return result;
  }

  if (update_failed && update_email) {
    const auto email_owner = get_user_by_email_result(user.email);
    if (email_owner.status == LookupStatus::FOUND && email_owner.value && email_owner.value->user_id != user.user_id) {
      return {MutationStatus::CONFLICT, std::nullopt, "EMAIL_CONFLICT"};
    }
  }
  if (result.status == MutationStatus::NOT_FOUND) {
    return result;
  }
  const auto stored = get_user_result(user.user_id);
  if (stored.status == LookupStatus::NOT_FOUND) {
    return {MutationStatus::NOT_FOUND, std::nullopt, "USER_NOT_FOUND"};
  }
  return {MutationStatus::STORAGE_ERROR, std::nullopt, "USER_UPDATE_FAILED"};
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
  auto page = list_files(name_pattern, type_filter, offset, limit);
  if (page.status != LookupStatus::FOUND || !page.value) {
    out_total = 0;
    return {};
  }
  out_total = page.value->total;
  return std::move(page.value->items);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LookupResult<FilePage> DatabasePool::list_files(const std::string& name_pattern,
                                                const std::string& type_filter,
                                                int offset,
                                                int limit) {
  LookupResult<FilePage> result{LookupStatus::STORAGE_ERROR, std::nullopt};
  if (!type_filter.empty() && type_filter != "audio" && type_filter != "image" && type_filter != "video" &&
      type_filter != "other") {
    return {LookupStatus::INVALID_DATA, std::nullopt};
  }
  std::string where;
  std::vector<std::string> params;
  if (!name_pattern.empty()) {
    where += " AND f.file_name LIKE ?";
    params.push_back("%" + name_pattern + "%");
  }
  if (!type_filter.empty()) {
    if (type_filter == "other") {
      where += " AND f.content_type NOT LIKE CONCAT(?, '/%') AND f.content_type NOT LIKE CONCAT(?, '/%') "
               "AND f.content_type NOT LIKE CONCAT(?, '/%')";
      params.insert(params.end(), {"audio", "image", "video"});
    } else {
      where += " AND f.content_type LIKE CONCAT(?, '/%')";
      params.push_back(type_filter);
    }
  }
  const bool committed = with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION WITH CONSISTENT SNAPSHOT"))
      return false;
    const auto count_result = connection.query("SELECT COUNT(*) FROM file_records f WHERE 1=1" + where, params);
    int total = 0;
    if (!count_result || count_result->rows.size() != 1 || count_result->rows[0].size() != 1 ||
        !parse_integer(count_result->rows[0][0], total) || total < 0) {
      if (count_result)
        result = {LookupStatus::INVALID_DATA, std::nullopt};
      return false;
    }
    auto query_params = params;
    query_params.push_back(std::to_string(limit));
    query_params.push_back(std::to_string(offset));
    const auto rows = connection.query("SELECT f.file_id, f.file_name, f.file_hash, f.file_size, "
                                       "f.content_type, f.chunk_size, f.created_at, f.music_id, f.uploaded_by "
                                       "FROM file_records f WHERE 1=1" +
                                         where + " ORDER BY f.file_id DESC LIMIT ? OFFSET ?",
                                       query_params);
    if (!rows)
      return false;
    FilePage page;
    page.total = total;
    page.offset = offset;
    page.limit = limit;
    page.items.reserve(rows->rows.size());
    for (const auto& row : rows->rows) {
      FileRecord record;
      if (row.size() != 9 || row[1].empty() || row[2].empty() || row[4].empty() || row[6].empty() ||
          !parse_integer(row[0], record.file_id) || record.file_id <= 0 || !parse_integer(row[3], record.file_size) ||
          !parse_integer(row[5], record.chunk_size) || record.chunk_size <= 0 ||
          !parse_integer(row[8], record.uploaded_by) || record.uploaded_by < 0 ||
          (!row[7].empty() && !parse_integer(row[7], record.music_id)) || !parse_mysql_utc_datetime(row[6])) {
        result = {LookupStatus::INVALID_DATA, std::nullopt};
        return false;
      }
      record.file_name = row[1];
      record.file_hash = row[2];
      record.content_type = row[4];
      record.created_at = row[6];
      page.items.push_back(std::move(record));
    }
    if (!connection.execute("COMMIT"))
      return false;
    result = {LookupStatus::FOUND, std::move(page)};
    return true;
  });
  if (!committed && result.status == LookupStatus::FOUND)
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  return result;
}

// IDatabasePool 与文件删除路由合同固定 permit、file_id、actor_id、can_delete_any 的公共参数顺序。
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MutationResult<FileDeletionPlan> DatabasePool::delete_file_owned(const ChunkLifecycleCoordinator::CleanupPermit& permit,
                                                                 int64_t file_id,
                                                                 int64_t actor_id,
                                                                 bool can_delete_any) {
  if (!accepts_cleanup_permit(permit)) {
    return {MutationStatus::INVALID_STATE, std::nullopt, "CLEANUP_PERMIT_INVALID"};
  }
  MutationResult<FileDeletionPlan> result{MutationStatus::STORAGE_ERROR, std::nullopt, "FILE_DELETE_FAILED"};
  const bool committed = with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION")) {
      return false;
    }
    const FileDeletionRequest request{file_id, actor_id, can_delete_any};
    FileDeletionState state;
    if (!read_file_deletion_preview(connection, request, state, result) ||
        !lock_preview_music(connection, state, result) || !lock_file_for_deletion(connection, request, state, result) ||
        !read_file_chunk_hashes(connection, state, result) || !find_unshared_file_chunks(connection, state, result) ||
        !delete_file_and_queue_chunks(connection, request, state) || !delete_unused_music_meta(connection, state)) {
      return false;
    }
    if (!connection.execute("COMMIT")) {
      return false;
    }
    result = {MutationStatus::OK, FileDeletionPlan{file_id, state.unshared_chunk_hashes.size()}, std::nullopt};
    return true;
  });
  if (!committed && result.status == MutationStatus::OK) {
    return {MutationStatus::STORAGE_ERROR, std::nullopt, "FILE_COMMIT_FAILED"};
  }
  return result;
}

MutationResult<std::vector<PendingChunkDeletion>> DatabasePool::claim_pending_chunk_deletions(
  std::size_t limit,
  std::chrono::system_clock::time_point stale_before) {
  if (limit == 0) {
    return {MutationStatus::INVALID_STATE, std::nullopt, "PENDING_LIMIT_INVALID"};
  }
  MutationResult<std::vector<PendingChunkDeletion>> result{MutationStatus::STORAGE_ERROR,
                                                           std::nullopt,
                                                           "PENDING_CLAIM_FAILED"};
  const bool committed = with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION")) {
      return false;
    }
    if (!connection.execute("UPDATE pending_chunk_deletions SET state = 'PENDING', claim_token = NULL, "
                            "claimed_at = NULL WHERE state = 'CLAIMED' AND claimed_at < ?",
                            {format_mysql_utc_datetime(stale_before)})) {
      return false;
    }
    const auto due =
      connection.query("SELECT chunk_hash, retry_count FROM pending_chunk_deletions WHERE state = 'PENDING' "
                       "AND next_attempt_at <= UTC_TIMESTAMP(6) ORDER BY next_attempt_at, chunk_hash "
                       "LIMIT ? FOR UPDATE SKIP LOCKED",
                       {std::to_string(limit)});
    if (!due) {
      return false;
    }
    std::vector<PendingChunkDeletion> claimed;
    claimed.reserve(due->rows.size());
    for (const auto& row : due->rows) {
      int retry_count = 0;
      if (row.size() != 2 || row[0].empty() || !parse_integer(row[1], retry_count) || retry_count < 0) {
        result = {MutationStatus::INVALID_STATE, std::nullopt, "PENDING_STATE_INVALID"};
        return false;
      }
      auto token = random_claim_token();
      const auto affected = connection.execute(
        "UPDATE pending_chunk_deletions SET state = 'CLAIMED', claim_token = ?, claimed_at = UTC_TIMESTAMP(6) "
        "WHERE chunk_hash = ? AND state = 'PENDING'",
        {token, row[0]});
      if (!affected || *affected != 1) {
        return false;
      }
      claimed.push_back({row[0], std::move(token), retry_count});
    }
    if (!connection.execute("COMMIT")) {
      return false;
    }
    result = {MutationStatus::OK, std::move(claimed), std::nullopt};
    return true;
  });
  if (!committed && result.status == MutationStatus::OK) {
    return {MutationStatus::STORAGE_ERROR, std::nullopt, "PENDING_CLAIM_COMMIT_FAILED"};
  }
  return result;
}

MutationResult<std::monostate> DatabasePool::complete_pending_chunk_deletion(const std::string& chunk_hash,
                                                                             const std::string& claim_token) {
  std::optional<int64_t> affected;
  const bool executed = with_connection([&](IConnection& connection) {
    affected = connection.execute("DELETE FROM pending_chunk_deletions WHERE chunk_hash = ? AND claim_token = ? "
                                  "AND state = 'CLAIMED'",
                                  {chunk_hash, claim_token});
    return affected.has_value();
  });
  if (!executed || !affected) {
    return {MutationStatus::STORAGE_ERROR, std::nullopt, "PENDING_COMPLETE_FAILED"};
  }
  if (*affected != 1) {
    return {MutationStatus::NOT_FOUND, std::nullopt, "PENDING_CLAIM_NOT_FOUND"};
  }
  return {MutationStatus::OK, std::monostate{}, std::nullopt};
}

MutationResult<std::monostate> DatabasePool::release_pending_chunk_deletion(const std::string& chunk_hash,
                                                                            const std::string& claim_token,
                                                                            const std::string& last_error) {
  std::optional<int64_t> affected;
  const bool executed = with_connection([&](IConnection& connection) {
    affected = connection.execute(
      "UPDATE pending_chunk_deletions SET state = 'PENDING', claim_token = NULL, claimed_at = NULL, "
      "next_attempt_at = DATE_ADD(UTC_TIMESTAMP(6), INTERVAL LEAST(POW(2, IF(retry_count >= 12, 12, retry_count + 1)), "
      "3600) SECOND), retry_count = IF(retry_count >= 2147483647, 2147483647, retry_count + 1), last_error = ? "
      "WHERE chunk_hash = ? AND claim_token = ? AND state = 'CLAIMED'",
      {last_error.substr(0, 512), chunk_hash, claim_token});
    return affected.has_value();
  });
  if (!executed || !affected) {
    return {MutationStatus::STORAGE_ERROR, std::nullopt, "PENDING_RELEASE_FAILED"};
  }
  if (*affected != 1) {
    return {MutationStatus::NOT_FOUND, std::nullopt, "PENDING_CLAIM_NOT_FOUND"};
  }
  return {MutationStatus::OK, std::monostate{}, std::nullopt};
}

LookupResult<bool> DatabasePool::has_chunk_references(const std::string& chunk_hash) {
  std::optional<QueryResult> result;
  const bool queried = with_connection([&](IConnection& connection) {
    result = connection.query("SELECT 1 FROM file_chunks WHERE chunk_hash = ? LIMIT 1", {chunk_hash});
    return result.has_value();
  });
  if (!queried || !result) {
    return {LookupStatus::STORAGE_ERROR, std::nullopt};
  }
  return {LookupStatus::FOUND, !result->rows.empty()};
}

MutationResult<std::monostate> DatabasePool::cancel_pending_chunk_deletion(const std::string& chunk_hash,
                                                                           const std::string& claim_token) {
  return complete_pending_chunk_deletion(chunk_hash, claim_token);
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

MutationResult<std::vector<Playlist>> DatabasePool::get_user_playlists(int64_t user_id, int64_t actor_id) {
  if (user_id != actor_id) {
    return {MutationStatus::OWNER_REQUIRED, std::nullopt, "OWNER_REQUIRED"};
  }
  auto conn = get_connection();
  if (!conn) {
    return {MutationStatus::STORAGE_ERROR, std::nullopt, "STORAGE"};
  }

  auto result =
    conn->query("SELECT p.playlist_id, p.user_id, p.name, p.description, "
                "(SELECT COUNT(*) FROM playlist_items pi WHERE pi.playlist_id = p.playlist_id) AS item_count, "
                "p.created_at "
                "FROM user_playlists p WHERE p.user_id = ? ORDER BY p.created_at DESC, p.playlist_id DESC",
                {std::to_string(user_id)});

  release_connection(std::move(conn));

  if (!result) {
    return {MutationStatus::STORAGE_ERROR, std::nullopt, "STORAGE"};
  }
  std::vector<Playlist> playlists;
  playlists.reserve(result->rows.size());
  for (const auto& row : result->rows) {
    if (row.size() != 6) {
      return {MutationStatus::INVALID_STATE, std::nullopt, "INVALID"};
    }
    Playlist p{};
    if (!parse_integer(row[0], p.playlist_id) || !parse_integer(row[1], p.user_id) || p.playlist_id <= 0 ||
        p.user_id != user_id || !parse_integer(row[4], p.item_count) || p.item_count < 0 ||
        !is_valid_playlist_text(row[2], row[3])) {
      return {MutationStatus::INVALID_STATE, std::nullopt, "INVALID"};
    }
    p.name = row[2];
    p.description = row[3];
    p.created_at = row[5];
    playlists.push_back(std::move(p));
  }
  return {MutationStatus::OK, std::move(playlists), std::nullopt};
}

MutationResult<Playlist> DatabasePool::create_playlist(const Playlist& playlist, int64_t actor_id) {
  if (playlist.user_id != actor_id) {
    return {MutationStatus::OWNER_REQUIRED, std::nullopt, "OWNER_REQUIRED"};
  }
  if (!is_valid_playlist_text(playlist.name, playlist.description)) {
    return {MutationStatus::INVALID_STATE, std::nullopt, "INVALID"};
  }
  MutationResult<Playlist> result;
  with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION"))
      return false;
    const auto user =
      connection.query("SELECT user_id FROM users WHERE user_id = ? FOR UPDATE", {std::to_string(actor_id)});
    if (!user)
      return false;
    if (user->rows.empty()) {
      result = {MutationStatus::USER_NOT_FOUND, std::nullopt, "USER_NOT_FOUND"};
      return false;
    }
    int64_t locked_user_id = 0;
    if (user->rows.size() != 1 || user->rows[0].size() != 1 || !parse_integer(user->rows[0][0], locked_user_id) ||
        locked_user_id != actor_id) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "INVALID_REQUEST"};
      return false;
    }
    const auto inserted = connection.execute("INSERT INTO user_playlists (user_id, name, description) VALUES (?, ?, ?)",
                                             {std::to_string(actor_id), playlist.name, playlist.description});
    if (!inserted || *inserted != 1)
      return false;
    const auto playlist_id = connection.last_insert_id();
    if (playlist_id <= 0)
      return false;
    auto created = read_playlist(connection, playlist_id, actor_id);
    if (created.status != MutationStatus::OK || !created.value) {
      result = std::move(created);
      return false;
    }
    if (!connection.execute("COMMIT"))
      return false;
    result = std::move(created);
    return true;
  });
  return result;
}

MutationResult<Playlist> DatabasePool::update_playlist(int64_t playlist_id,
                                                       int64_t actor_id,
                                                       const std::string& name,
                                                       const std::string& description) {
  if (!is_valid_playlist_text(name, description)) {
    return {MutationStatus::INVALID_STATE, std::nullopt, "INVALID"};
  }
  MutationResult<Playlist> result;
  with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION"))
      return false;
    const auto owner_status =
      lock_playlist_owner(connection, PlaylistOwnerRequest{.playlist_id = playlist_id, .actor_id = actor_id});
    if (owner_status != MutationStatus::OK) {
      result = {owner_status, std::nullopt, std::nullopt};
      return false;
    }
    const auto items = lock_playlist_items(connection, playlist_id);
    if (items.status != MutationStatus::OK) {
      result = {items.status, std::nullopt, items.detail};
      return false;
    }
    const auto updated = connection.execute("UPDATE user_playlists SET name = ?, description = ? WHERE playlist_id = ?",
                                            {name, description, std::to_string(playlist_id)});
    if (!updated || *updated != 1)
      return false;
    auto playlist = read_playlist(connection, playlist_id, actor_id);
    if (playlist.status != MutationStatus::OK || !playlist.value) {
      result = std::move(playlist);
      return false;
    }
    if (!connection.execute("COMMIT"))
      return false;
    result = std::move(playlist);
    return true;
  });
  return result;
}

MutationResult<std::monostate> DatabasePool::delete_playlist(int64_t playlist_id, int64_t actor_id) {
  MutationResult<std::monostate> result;
  with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION"))
      return false;
    const auto owner_status =
      lock_playlist_owner(connection, PlaylistOwnerRequest{.playlist_id = playlist_id, .actor_id = actor_id});
    if (owner_status != MutationStatus::OK) {
      result = {owner_status, std::nullopt, std::nullopt};
      return false;
    }
    const auto items = lock_playlist_items(connection, playlist_id);
    if (items.status != MutationStatus::OK) {
      result = {items.status, std::nullopt, items.detail};
      return false;
    }
    const auto deleted =
      connection.execute("DELETE FROM user_playlists WHERE playlist_id = ?", {std::to_string(playlist_id)});
    if (!deleted || *deleted != 1 || !connection.execute("COMMIT"))
      return false;
    result = {MutationStatus::OK, std::monostate{}, std::nullopt};
    return true;
  });
  return result;
}

// IDatabasePool 与歌单路由合同固定 playlist_id、actor_id 的公共参数顺序。
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MutationResult<std::vector<PlaylistItem>> DatabasePool::get_playlist_items(int64_t playlist_id, int64_t actor_id) {
  MutationResult<std::vector<PlaylistItem>> result;
  with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION WITH CONSISTENT SNAPSHOT"))
      return false;
    const auto owner =
      connection.query("SELECT user_id FROM user_playlists WHERE playlist_id = ?", {std::to_string(playlist_id)});
    if (!owner)
      return false;
    if (owner->rows.empty()) {
      result = {MutationStatus::NOT_FOUND, std::nullopt, "PLAYLIST_NOT_FOUND"};
      return false;
    }
    int64_t owner_id = 0;
    if (owner->rows.size() != 1 || owner->rows[0].size() != 1 || !parse_integer(owner->rows[0][0], owner_id) ||
        owner_id <= 0) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "INVALID_REQUEST"};
      return false;
    }
    if (owner_id != actor_id) {
      result = {MutationStatus::OWNER_REQUIRED, std::nullopt, "PLAYLIST_OWNER_REQUIRED"};
      return false;
    }
    const auto items = connection.query("SELECT pi.id, pi.playlist_id, pi.music_id, pi.sort_order, pi.added_at, "
                                        "m.title, m.artist, "
                                        "(SELECT f.file_hash FROM file_records f "
                                        "WHERE f.music_id = m.music_id AND f.content_type LIKE 'audio/%' "
                                        "ORDER BY f.file_id ASC LIMIT 1) AS file_hash "
                                        "FROM playlist_items pi "
                                        "STRAIGHT_JOIN music_meta m ON m.music_id = pi.music_id "
                                        "WHERE pi.playlist_id = ? ORDER BY pi.sort_order ASC, pi.id ASC",
                                        {std::to_string(playlist_id)});
    if (!items)
      return false;
    std::vector<PlaylistItem> parsed_items;
    parsed_items.reserve(items->rows.size());
    for (const auto& row : items->rows) {
      if (row.size() != 8) {
        result = {MutationStatus::INVALID_STATE, std::nullopt, "INVALID_REQUEST"};
        return false;
      }
      PlaylistItem item{};
      if (!parse_integer(row[0], item.id) || !parse_integer(row[1], item.playlist_id) ||
          !parse_integer(row[2], item.music_id) || !parse_integer(row[3], item.sort_order) || item.id <= 0 ||
          item.playlist_id != playlist_id || item.music_id <= 0 || item.sort_order < 0) {
        result = {MutationStatus::INVALID_STATE, std::nullopt, "INVALID_REQUEST"};
        return false;
      }
      item.added_at = row[4];
      item.title = row[5];
      item.artist = row[6];
      item.file_hash = row[7];
      parsed_items.push_back(std::move(item));
    }
    if (!connection.execute("COMMIT"))
      return false;
    result = {MutationStatus::OK, std::move(parsed_items), std::nullopt};
    return true;
  });
  return result;
}

// IDatabasePool 与歌单路由合同固定 playlist_id、actor_id、music_id 的公共参数顺序。
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
MutationResult<std::monostate> DatabasePool::add_playlist_item(int64_t playlist_id,
                                                               // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                                               int64_t actor_id,
                                                               int64_t music_id) {
  MutationResult<std::monostate> result;
  with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION"))
      return false;
    const auto music =
      connection.query("SELECT music_id FROM music_meta WHERE music_id = ? FOR UPDATE", {std::to_string(music_id)});
    if (!music)
      return false;
    int64_t locked_music_id = 0;
    if (music->rows.empty()) {
      result = {MutationStatus::NOT_FOUND, std::nullopt, "MUSIC_NOT_FOUND"};
      return false;
    }
    if (music->rows.size() != 1 || music->rows[0].size() != 1 || !parse_integer(music->rows[0][0], locked_music_id) ||
        locked_music_id != music_id) {
      result = {MutationStatus::INVALID_STATE, std::nullopt, "INVALID_REQUEST"};
      return false;
    }
    const auto owner_status =
      lock_playlist_owner(connection, PlaylistOwnerRequest{.playlist_id = playlist_id, .actor_id = actor_id});
    if (owner_status != MutationStatus::OK) {
      result = {owner_status, std::nullopt, std::nullopt};
      return false;
    }
    const auto items = lock_playlist_items(connection, playlist_id);
    if (items.status != MutationStatus::OK || !items.value) {
      result = {items.status, std::nullopt, items.detail};
      return false;
    }
    int next_order = 0;
    bool duplicate = false;
    for (const auto& item : *items.value) {
      if (item.music_id == music_id)
        duplicate = true;
      next_order = std::max(next_order, item.sort_order + 1);
    }
    if (duplicate) {
      result = {MutationStatus::CONFLICT, std::nullopt, "PLAYLIST_ORDER_CONFLICT"};
      return false;
    }
    const auto inserted =
      connection.execute("INSERT INTO playlist_items (playlist_id, music_id, sort_order) "
                         "VALUES (?, ?, ?)",
                         {std::to_string(playlist_id), std::to_string(music_id), std::to_string(next_order)});
    if (!inserted || *inserted != 1 || !connection.execute("COMMIT"))
      return false;
    result = {MutationStatus::OK, std::monostate{}, std::nullopt};
    return true;
  });
  return result;
}

// IDatabasePool 与歌单路由合同固定 playlist_id、actor_id、music_id 的公共参数顺序。
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters,readability-function-cognitive-complexity)
MutationResult<std::monostate> DatabasePool::remove_playlist_item(
  int64_t playlist_id,
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  int64_t actor_id,
  int64_t music_id) {
  MutationResult<std::monostate> result;
  with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION"))
      return false;
    const auto owner_status =
      lock_playlist_owner(connection, PlaylistOwnerRequest{.playlist_id = playlist_id, .actor_id = actor_id});
    if (owner_status != MutationStatus::OK) {
      result = {owner_status, std::nullopt, std::nullopt};
      return false;
    }
    const auto items = lock_playlist_items(connection, playlist_id);
    if (items.status != MutationStatus::OK || !items.value) {
      result = {items.status, std::nullopt, items.detail};
      return false;
    }
    bool found = false;
    std::vector<int64_t> remaining_ids;
    for (const auto& item : *items.value) {
      if (item.music_id == music_id) {
        found = true;
      } else {
        remaining_ids.push_back(item.music_id);
      }
    }
    if (!found) {
      result = {MutationStatus::NOT_FOUND, std::nullopt, "MUSIC_NOT_FOUND"};
      return false;
    }
    const auto deleted = connection.execute("DELETE FROM playlist_items WHERE playlist_id = ? AND music_id = ?",
                                            {std::to_string(playlist_id), std::to_string(music_id)});
    if (!deleted || *deleted != 1)
      return false;
    for (std::size_t index = 0; index < remaining_ids.size(); ++index) {
      const auto rewritten =
        connection.execute("UPDATE playlist_items SET sort_order = ? "
                           "WHERE playlist_id = ? AND music_id = ?",
                           {std::to_string(index), std::to_string(playlist_id), std::to_string(remaining_ids[index])});
      if (!rewritten || *rewritten < 0 || *rewritten > 1)
        return false;
    }
    if (!connection.execute("COMMIT"))
      return false;
    result = {MutationStatus::OK, std::monostate{}, std::nullopt};
    return true;
  });
  return result;
}

MutationResult<std::monostate> DatabasePool::reorder_playlist_items(int64_t playlist_id,
                                                                    int64_t actor_id,
                                                                    const std::vector<int64_t>& music_ids) {
  MutationResult<std::monostate> result;
  with_connection([&](IConnection& connection) {
    if (!connection.execute("START TRANSACTION"))
      return false;
    const auto owner_status =
      lock_playlist_owner(connection, PlaylistOwnerRequest{.playlist_id = playlist_id, .actor_id = actor_id});
    if (owner_status != MutationStatus::OK) {
      result = {owner_status, std::nullopt, std::nullopt};
      return false;
    }
    const auto items = lock_playlist_items(connection, playlist_id);
    if (items.status != MutationStatus::OK || !items.value) {
      result = {items.status, std::nullopt, items.detail};
      return false;
    }
    std::unordered_set<int64_t> existing;
    for (const auto& item : *items.value) existing.insert(item.music_id);
    const std::unordered_set<int64_t> requested(music_ids.begin(), music_ids.end());
    if (requested.size() != music_ids.size() || requested != existing) {
      result = {MutationStatus::CONFLICT, std::nullopt, "PLAYLIST_ORDER_CONFLICT"};
      return false;
    }
    for (std::size_t index = 0; index < music_ids.size(); ++index) {
      const auto updated =
        connection.execute("UPDATE playlist_items SET sort_order = ? "
                           "WHERE playlist_id = ? AND music_id = ?",
                           {std::to_string(index), std::to_string(playlist_id), std::to_string(music_ids[index])});
      if (!updated || *updated != 1)
        return false;
    }
    if (!connection.execute("COMMIT"))
      return false;
    result = {MutationStatus::OK, std::monostate{}, std::nullopt};
    return true;
  });
  return result;
}

} // namespace hps
