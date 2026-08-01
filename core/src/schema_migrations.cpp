#include "schema_migrations.h"

#include "iconnection.h"

#include <string>

namespace hps {
namespace {

constexpr std::string_view kMigrationKey = "step_19_vip_lifecycle_v2";

bool schema_object_exists(IConnection& connection, const std::string& sql, const std::string& name) {
  const auto result = connection.query(sql, {name});
  return result.has_value() && !result->rows.empty();
}

bool fail_stage(std::string& detail, std::string_view stage) {
  detail = stage;
  return false;
}

bool execute_ddl(IConnection& connection, std::string& detail) {
  if (!connection.execute("CREATE TABLE IF NOT EXISTS schema_migrations ("
                          "migration_key VARCHAR(128) PRIMARY KEY, applied_at DATETIME(6) NOT NULL) ENGINE=InnoDB")) {
    return fail_stage(detail, "ddl_schema_migrations");
  }
  if (!connection.execute(
        "CREATE TABLE IF NOT EXISTS pending_chunk_deletions ("
        "chunk_hash VARCHAR(64) PRIMARY KEY, state ENUM('PENDING','CLAIMED') NOT NULL DEFAULT 'PENDING', "
        "claim_token VARCHAR(64) NULL, claimed_at DATETIME(6) NULL, retry_count INT NOT NULL DEFAULT 0, "
        "next_attempt_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6), last_error VARCHAR(512) NULL, "
        "created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6), "
        "updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6), "
        "INDEX idx_pending_chunk_due (state, next_attempt_at)) ENGINE=InnoDB")) {
    return fail_stage(detail, "ddl_pending_chunk_deletions");
  }

  constexpr std::string_view column_query =
    "SELECT 1 FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'users' "
    "AND column_name = ? LIMIT 1";
  if (!schema_object_exists(connection, std::string(column_query), "vip_expires_at") &&
      !connection.execute("ALTER TABLE users ADD COLUMN vip_expires_at DATETIME(6) NULL AFTER email")) {
    return fail_stage(detail, "ddl_vip_expires_at");
  }
  if (!schema_object_exists(connection, std::string(column_query), "admin_slot") &&
      !connection.execute("ALTER TABLE users ADD COLUMN admin_slot TINYINT GENERATED ALWAYS AS "
                          "(CASE WHEN role = 3 THEN 1 ELSE NULL END) STORED")) {
    return fail_stage(detail, "ddl_admin_slot");
  }

  constexpr std::string_view index_query =
    "SELECT 1 FROM information_schema.statistics WHERE table_schema = DATABASE() AND table_name = 'users' "
    "AND index_name = ? LIMIT 1";
  if (!connection.execute("UPDATE users SET email = NULL WHERE email = ''")) {
    return fail_stage(detail, "normalize_empty_user_email");
  }
  const auto duplicate_email =
    connection.query("SELECT 1 FROM users WHERE email IS NOT NULL GROUP BY email HAVING COUNT(*) > 1 LIMIT 1");
  if (!duplicate_email) {
    return fail_stage(detail, "email_duplicate_check");
  }
  if (!duplicate_email->rows.empty()) {
    return fail_stage(detail, "duplicate_user_email");
  }
  if (!schema_object_exists(connection, std::string(index_query), "uk_users_email") &&
      !connection.execute("ALTER TABLE users ADD UNIQUE KEY uk_users_email (email)")) {
    return fail_stage(detail, "ddl_unique_user_email");
  }
  if (!schema_object_exists(connection, std::string(index_query), "uk_users_single_admin") &&
      !connection.execute("ALTER TABLE users ADD UNIQUE KEY uk_users_single_admin (admin_slot)")) {
    return fail_stage(detail, "ddl_single_admin_index");
  }
  if (!schema_object_exists(connection, std::string(index_query), "idx_users_vip_expires_at") &&
      !connection.execute("ALTER TABLE users ADD INDEX idx_users_vip_expires_at (vip_expires_at)")) {
    return fail_stage(detail, "ddl_vip_expires_index");
  }
  return true;
}

bool execute_backfill_transaction(IConnection& connection,
                                  std::chrono::system_clock::time_point now,
                                  std::string& detail) {
  const auto applied_at = try_format_mysql_utc_datetime(now);
  if (!applied_at) {
    return fail_stage(detail, "migration_timestamp_out_of_range");
  }
  if (!connection.execute("START TRANSACTION")) {
    return fail_stage(detail, "migration_transaction_begin");
  }
  const auto marker = connection.query("SELECT migration_key FROM schema_migrations WHERE migration_key = ? FOR UPDATE",
                                       {std::string(kMigrationKey)});
  if (!marker) {
    return fail_stage(detail, "migration_marker_lock");
  }
  if (!marker->rows.empty()) {
    if (!connection.execute("COMMIT")) {
      return fail_stage(detail, "migration_commit");
    }
    return true;
  }

  const auto invalid_role = connection.query("SELECT 1 FROM users WHERE role NOT IN (0, 1, 2, 3) LIMIT 1");
  if (!invalid_role) {
    return fail_stage(detail, "invalid_user_role_check");
  }
  if (!invalid_role->rows.empty()) {
    return fail_stage(detail, "invalid_user_role");
  }
  if (!connection.execute(
        "UPDATE users SET vip_expires_at = NULL WHERE role IN (0, 1, 3) AND vip_expires_at IS NOT NULL")) {
    return fail_stage(detail, "vip_backfill");
  }
  if (!connection.execute(
        "UPDATE users SET role = 1, vip_expires_at = NULL WHERE role = 2 AND vip_expires_at IS NULL")) {
    return fail_stage(detail, "vip_backfill");
  }
  if (!connection.execute("INSERT INTO schema_migrations (migration_key, applied_at) VALUES (?, ?)",
                          {std::string(kMigrationKey), *applied_at})) {
    return fail_stage(detail, "migration_marker_insert");
  }
  if (!connection.execute("COMMIT")) {
    return fail_stage(detail, "migration_commit");
  }
  return true;
}

} // namespace

MutationResult<std::monostate> run_schema_migrations(IDatabasePool& database,
                                                     std::chrono::system_clock::time_point now) {
  MutationResult<std::monostate> result;
  std::string detail;
  bool ddl_succeeded = false;
  const bool ddl_connection_succeeded = database.with_connection([&detail, &ddl_succeeded](IConnection& connection) {
    ddl_succeeded = execute_ddl(connection, detail);
    return true;
  });
  if (!ddl_connection_succeeded || !ddl_succeeded) {
    result.detail = detail.empty() ? "database_connection" : detail;
    return result;
  }
  if (database.with_connection(
        [now, &detail](IConnection& connection) { return execute_backfill_transaction(connection, now, detail); })) {
    result.status = MutationStatus::OK;
    result.value = std::monostate{};
  } else {
    result.detail = detail.empty() ? "database_connection" : detail;
  }
  return result;
}

} // namespace hps
