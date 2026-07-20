#pragma once

#include "db_config.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hps {

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
};

class IConnection {
public:
  virtual ~IConnection() = default;

  virtual bool connect(const DbConfig& config) = 0;
  virtual bool is_open() const = 0;
  virtual bool ping() = 0;

  // 参数化查询：params 为空走普通 query，非空走 prepared statement
  virtual std::optional<QueryResult> query(const std::string& sql, const std::vector<std::string>& params = {}) = 0;

  // 返回 affected_rows，适用于 INSERT/UPDATE/DELETE
  virtual std::optional<int64_t> execute(const std::string& sql, const std::vector<std::string>& params = {}) = 0;

  // 返回最近一次 INSERT 的自增 ID（默认 0，由具体实现覆盖）
  virtual int64_t last_insert_id() const { return 0; }

  virtual void close() = 0;
};

} // namespace hps
