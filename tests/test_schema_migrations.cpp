#include "database_pool.h"
#include "mock_connection.h"
#include "schema_migrations.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace hps {
namespace {

struct MigrationDatabase {
  std::vector<std::string> statements;
  bool vip_column_exists{false};
  bool admin_slot_exists{false};
  bool admin_index_exists{false};
  bool email_index_exists{false};
  bool vip_index_exists{false};
  bool duplicate_nonempty_email{false};
  bool invalid_user_role{false};
  int empty_email_count{0};
  int email_normalization_count{0};
  bool marker_committed{false};
  bool marker_pending{false};
  bool backfill_pending{false};
  bool transaction_open{false};
  bool fail_first_vip_alter{false};
  bool fail_backfill_once{false};
  bool fail_marker_insert_once{false};
  bool fail_commit_once{false};
  int committed_backfill_count{0};
  int marker_commit_count{0};
  int rollback_count{0};
  std::unique_ptr<DatabasePool> pool;

  MigrationDatabase() {
    pool = std::make_unique<DatabasePool>([this]() {
      auto connection = std::make_unique<MockConnection>();
      connection->query_hook = [this](const std::string& sql,
                                      const std::vector<std::string>& params) -> std::optional<QueryResult> {
        statements.push_back(sql);
        if (sql.find("information_schema.columns") != std::string::npos) {
          if (!params.empty() && params[0] == "vip_expires_at") {
            return presence(vip_column_exists);
          }
          if (!params.empty() && params[0] == "admin_slot") {
            return presence(admin_slot_exists);
          }
        }
        if (sql.find("information_schema.statistics") != std::string::npos) {
          if (!params.empty() && params[0] == "uk_users_single_admin") {
            return presence(admin_index_exists);
          }
          if (!params.empty() && params[0] == "idx_users_vip_expires_at") {
            return presence(vip_index_exists);
          }
          if (!params.empty() && params[0] == "uk_users_email") {
            return presence(email_index_exists);
          }
        }
        if (sql.find("HAVING COUNT(*) > 1") != std::string::npos) {
          return presence(duplicate_nonempty_email);
        }
        if (sql == "SELECT 1 FROM users WHERE role NOT IN (0, 1, 2, 3) LIMIT 1") {
          return presence(invalid_user_role);
        }
        if (sql.find("FROM schema_migrations") != std::string::npos) {
          return presence(marker_committed);
        }
        return QueryResult{};
      };
      connection->execute_hook = [this](const std::string& sql,
                                        const std::vector<std::string>&) -> std::optional<int64_t> {
        statements.push_back(sql);
        if (sql == "START TRANSACTION") {
          transaction_open = true;
          marker_pending = false;
          backfill_pending = false;
        } else if (sql == "ROLLBACK") {
          ++rollback_count;
          transaction_open = false;
          marker_pending = false;
          backfill_pending = false;
        } else if (sql == "COMMIT") {
          if (fail_commit_once) {
            fail_commit_once = false;
            return std::nullopt;
          }
          if (!transaction_open) {
            return std::nullopt;
          }
          if (backfill_pending) {
            ++committed_backfill_count;
          }
          if (marker_pending) {
            marker_committed = true;
            ++marker_commit_count;
          }
          transaction_open = false;
          marker_pending = false;
          backfill_pending = false;
        } else if (sql.find("ADD COLUMN vip_expires_at") != std::string::npos) {
          if (fail_first_vip_alter) {
            fail_first_vip_alter = false;
            return std::nullopt;
          }
          vip_column_exists = true;
        } else if (sql.find("ADD COLUMN admin_slot") != std::string::npos) {
          admin_slot_exists = true;
        } else if (sql.find("ADD UNIQUE KEY uk_users_single_admin") != std::string::npos) {
          admin_index_exists = true;
        } else if (sql.find("ADD INDEX idx_users_vip_expires_at") != std::string::npos) {
          vip_index_exists = true;
        } else if (sql.find("ADD UNIQUE KEY uk_users_email") != std::string::npos) {
          email_index_exists = true;
        } else if (sql == "UPDATE users SET email = NULL WHERE email = ''") {
          ++email_normalization_count;
          empty_email_count = 0;
        } else if (sql.find("UPDATE users SET vip_expires_at") != std::string::npos) {
          if (fail_backfill_once) {
            fail_backfill_once = false;
            return std::nullopt;
          }
          backfill_pending = true;
        } else if (sql.find("INSERT INTO schema_migrations") != std::string::npos) {
          if (fail_marker_insert_once) {
            fail_marker_insert_once = false;
            return std::nullopt;
          }
          marker_pending = true;
        }
        return 1;
      };
      return connection;
    });
    DbConfig config;
    config.pool_size = 1;
    if (!pool->init(config)) {
      throw std::runtime_error("测试数据库池初始化失败");
    }
  }

  static QueryResult presence(bool exists) {
    QueryResult result;
    if (exists) {
      result.rows.push_back({"1"});
    }
    return result;
  }
};

constexpr auto kNow = std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};

TEST(SchemaMigrationsTest, BackfillsHistoricalVipAndMarkerOnlyOnceInOneTransaction) {
  MigrationDatabase database;

  EXPECT_EQ(run_schema_migrations(*database.pool, kNow).status, MutationStatus::OK);
  EXPECT_EQ(run_schema_migrations(*database.pool, kNow + std::chrono::hours{24}).status, MutationStatus::OK);

  EXPECT_EQ(database.committed_backfill_count, 1);
  EXPECT_EQ(database.marker_commit_count, 1);
  EXPECT_TRUE(database.marker_committed);
  EXPECT_FALSE(database.transaction_open);
}

TEST(SchemaMigrationsTest, RepairsHistoricalRoleExpiryViolationsInTheMarkerTransaction) {
  MigrationDatabase database;

  ASSERT_EQ(run_schema_migrations(*database.pool, kNow).status, MutationStatus::OK);

  const auto clear_non_vip =
    std::ranges::find(database.statements,
                      "UPDATE users SET vip_expires_at = NULL WHERE role IN (0, 1, 3) AND vip_expires_at IS NOT NULL");
  const auto demote_invalid_vip =
    std::ranges::find(database.statements,
                      "UPDATE users SET role = 1, vip_expires_at = NULL WHERE role = 2 AND vip_expires_at IS NULL");
  const auto marker = std::ranges::find_if(database.statements, [](const std::string& sql) {
    return sql.find("INSERT INTO schema_migrations") != std::string::npos;
  });
  ASSERT_NE(clear_non_vip, database.statements.end());
  ASSERT_NE(demote_invalid_vip, database.statements.end());
  ASSERT_NE(marker, database.statements.end());
  EXPECT_LT(clear_non_vip, marker);
  EXPECT_LT(demote_invalid_vip, marker);
}

TEST(SchemaMigrationsTest, FailsClosedWhenHistoricalUserRoleIsUnknown) {
  MigrationDatabase database;
  database.invalid_user_role = true;
  ASSERT_TRUE(database.invalid_user_role);

  const auto result = run_schema_migrations(*database.pool, kNow);

  EXPECT_EQ(result.status, MutationStatus::STORAGE_ERROR);
  EXPECT_EQ(result.detail, "invalid_user_role");
  EXPECT_FALSE(database.marker_committed);
  EXPECT_GT(database.rollback_count, 0);
}

TEST(SchemaMigrationsTest, KeepsDdlOutsideTransactionsAndBuildsSingleAdminConstraintInOrder) {
  MigrationDatabase database;

  ASSERT_EQ(run_schema_migrations(*database.pool, kNow).status, MutationStatus::OK);

  bool transaction_open = false;
  for (const auto& sql : database.statements) {
    if (sql == "START TRANSACTION") {
      transaction_open = true;
    } else if (sql == "COMMIT" || sql == "ROLLBACK") {
      transaction_open = false;
    } else if (sql.starts_with("CREATE TABLE") || sql.starts_with("ALTER TABLE")) {
      EXPECT_FALSE(transaction_open);
    }
  }
  const auto admin_column = std::ranges::find_if(database.statements, [](const std::string& sql) {
    return sql.find("ADD COLUMN admin_slot") != std::string::npos &&
           sql.find("CASE WHEN role = 3 THEN 1 ELSE NULL END") != std::string::npos;
  });
  const auto admin_index = std::ranges::find_if(database.statements, [](const std::string& sql) {
    return sql.find("ADD UNIQUE KEY uk_users_single_admin (admin_slot)") != std::string::npos;
  });
  ASSERT_NE(admin_column, database.statements.end());
  ASSERT_NE(admin_index, database.statements.end());
  EXPECT_LT(admin_column, admin_index);
}

TEST(SchemaMigrationsTest, NormalizesMultipleEmptyEmailsAndBuildsUniqueEmailIndexOutsideTransaction) {
  MigrationDatabase database;
  database.empty_email_count = 3;
  ASSERT_EQ(database.empty_email_count, 3);

  ASSERT_EQ(run_schema_migrations(*database.pool, kNow).status, MutationStatus::OK);

  EXPECT_EQ(database.empty_email_count, 0);
  EXPECT_EQ(database.email_normalization_count, 1);
  EXPECT_TRUE(database.email_index_exists);
  const auto normalize = std::ranges::find(database.statements, "UPDATE users SET email = NULL WHERE email = ''");
  const auto duplicate_check = std::ranges::find_if(database.statements, [](const std::string& sql) {
    return sql.find("HAVING COUNT(*) > 1") != std::string::npos;
  });
  const auto email_index = std::ranges::find_if(database.statements, [](const std::string& sql) {
    return sql.find("ADD UNIQUE KEY uk_users_email (email)") != std::string::npos;
  });
  ASSERT_NE(normalize, database.statements.end());
  ASSERT_NE(duplicate_check, database.statements.end());
  ASSERT_NE(email_index, database.statements.end());
  EXPECT_LT(normalize, duplicate_check);
  EXPECT_LT(duplicate_check, email_index);
  EXPECT_EQ(std::ranges::find(database.statements.begin(), email_index, "START TRANSACTION"), email_index);
}

TEST(SchemaMigrationsTest, DuplicateHistoricalNonemptyEmailFailsClosedWithoutPretendingDdlRollback) {
  MigrationDatabase database;
  database.duplicate_nonempty_email = true;
  ASSERT_TRUE(database.duplicate_nonempty_email);

  const auto failed = run_schema_migrations(*database.pool, kNow);

  EXPECT_EQ(failed.status, MutationStatus::STORAGE_ERROR);
  EXPECT_EQ(failed.detail, "duplicate_user_email");
  EXPECT_FALSE(database.email_index_exists);
  EXPECT_EQ(std::ranges::count(database.statements, "START TRANSACTION"), 0);
  EXPECT_EQ(std::ranges::count(database.statements, "ROLLBACK"), 0);
}

TEST(SchemaMigrationsTest, EmailUniqueIndexCreationIsIdempotent) {
  MigrationDatabase database;

  EXPECT_EQ(run_schema_migrations(*database.pool, kNow).status, MutationStatus::OK);
  EXPECT_EQ(run_schema_migrations(*database.pool, kNow).status, MutationStatus::OK);

  EXPECT_TRUE(database.email_index_exists);
  EXPECT_EQ(std::ranges::count_if(database.statements,
                                  [](const std::string& sql) {
                                    return sql.find("ADD UNIQUE KEY uk_users_email (email)") != std::string::npos;
                                  }),
            1);
}

TEST(SchemaMigrationsTest, RetriesIdempotentDdlAfterInterruptedRun) {
  MigrationDatabase database;
  database.fail_first_vip_alter = true;
  ASSERT_TRUE(database.fail_first_vip_alter);

  const auto failed = run_schema_migrations(*database.pool, kNow);
  EXPECT_EQ(failed.status, MutationStatus::STORAGE_ERROR);
  EXPECT_EQ(failed.detail, "ddl_vip_expires_at");
  EXPECT_EQ(database.committed_backfill_count, 0);
  EXPECT_EQ(run_schema_migrations(*database.pool, kNow).status, MutationStatus::OK);

  EXPECT_TRUE(database.vip_column_exists);
  EXPECT_TRUE(database.admin_slot_exists);
  EXPECT_TRUE(database.admin_index_exists);
  EXPECT_TRUE(database.vip_index_exists);
  EXPECT_EQ(database.committed_backfill_count, 1);
}

TEST(SchemaMigrationsTest, RollsBackBackfillFailureAndRecoversOnNextRun) {
  MigrationDatabase database;
  database.fail_backfill_once = true;
  ASSERT_TRUE(database.fail_backfill_once);

  const auto failed = run_schema_migrations(*database.pool, kNow);
  EXPECT_EQ(failed.status, MutationStatus::STORAGE_ERROR);
  EXPECT_EQ(failed.detail, "vip_backfill");
  EXPECT_FALSE(database.marker_committed);
  EXPECT_FALSE(database.transaction_open);
  EXPECT_GT(database.rollback_count, 0);
  EXPECT_EQ(run_schema_migrations(*database.pool, kNow).status, MutationStatus::OK);
  EXPECT_EQ(database.committed_backfill_count, 1);
}

TEST(SchemaMigrationsTest, RollsBackMarkerInsertFailureAndRecoversOnNextRun) {
  MigrationDatabase database;
  database.fail_marker_insert_once = true;
  ASSERT_TRUE(database.fail_marker_insert_once);

  const auto failed = run_schema_migrations(*database.pool, kNow);
  EXPECT_EQ(failed.status, MutationStatus::STORAGE_ERROR);
  EXPECT_EQ(failed.detail, "migration_marker_insert");
  EXPECT_FALSE(database.marker_committed);
  EXPECT_FALSE(database.transaction_open);
  EXPECT_GT(database.rollback_count, 0);
  EXPECT_EQ(run_schema_migrations(*database.pool, kNow).status, MutationStatus::OK);
  EXPECT_EQ(database.committed_backfill_count, 1);
}

TEST(SchemaMigrationsTest, RollsBackCommitFailureAndRecoversWithoutDoubleBackfill) {
  MigrationDatabase database;
  database.fail_commit_once = true;
  ASSERT_TRUE(database.fail_commit_once);

  const auto failed = run_schema_migrations(*database.pool, kNow);
  EXPECT_EQ(failed.status, MutationStatus::STORAGE_ERROR);
  EXPECT_EQ(failed.detail, "migration_commit");
  EXPECT_FALSE(database.marker_committed);
  EXPECT_FALSE(database.transaction_open);
  EXPECT_GT(database.rollback_count, 0);
  EXPECT_EQ(run_schema_migrations(*database.pool, kNow).status, MutationStatus::OK);
  EXPECT_EQ(database.committed_backfill_count, 1);
  EXPECT_EQ(database.marker_commit_count, 1);
}

TEST(SchemaMigrationsTest, RollsBackCommitFailureWhenMarkerAlreadyExists) {
  MigrationDatabase database;
  ASSERT_EQ(run_schema_migrations(*database.pool, kNow).status, MutationStatus::OK);
  database.fail_commit_once = true;
  ASSERT_TRUE(database.fail_commit_once);
  const auto rollback_before = database.rollback_count;

  const auto failed = run_schema_migrations(*database.pool, kNow);
  EXPECT_EQ(failed.status, MutationStatus::STORAGE_ERROR);
  EXPECT_EQ(failed.detail, "migration_commit");
  EXPECT_FALSE(database.transaction_open);
  EXPECT_GT(database.rollback_count, rollback_before);
  EXPECT_EQ(run_schema_migrations(*database.pool, kNow).status, MutationStatus::OK);
  EXPECT_EQ(database.committed_backfill_count, 1);
}

} // namespace
} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
