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

  virtual std::optional<User> get_user(int64_t user_id) = 0;
  virtual bool create_user(const User& user) = 0;

  virtual bool store_file_record(const FileRecord& record) = 0;
  virtual std::optional<FileRecord> get_file_record(int64_t file_id) = 0;
  virtual std::optional<FileRecord> get_file_record_by_hash(const std::string& hash) = 0;
  virtual std::vector<FileRecord> search_files(const std::string& name_pattern, int offset, int limit) = 0;

  virtual bool store_file_chunks(const std::vector<FileChunkRecord>& chunks) = 0;
  virtual std::vector<FileChunkRecord> get_file_chunks(const std::string& file_hash) = 0;
  virtual bool chunk_exists(const std::string& chunk_hash) = 0;

  virtual std::optional<AuthUser> get_auth_user(const std::string& username) = 0;
  virtual bool verify_password(const std::string& username, const std::string& password) = 0;
};

} // namespace hps
