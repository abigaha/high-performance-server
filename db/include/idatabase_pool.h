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

  virtual bool log_download(const DownloadLog& log) = 0;
  virtual std::vector<DownloadLog> get_download_history(int64_t user_id) = 0;

  virtual bool store_file_meta(const FileMeta& meta) = 0;
  virtual std::optional<FileMeta> get_file_meta(const std::string& hash) = 0;
};

} // namespace hps
