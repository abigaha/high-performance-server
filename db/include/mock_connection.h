#pragma once

#include "db_config.h"
#include "iconnection.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hps {

class MockConnection : public IConnection {
public:
  bool connect_result = true;
  bool ping_result = true;
  bool is_open_result = true;

  std::optional<QueryResult> query_result;
  std::optional<int64_t> execute_result;

  std::function<std::optional<QueryResult>(const std::string&, const std::vector<std::string>&)> query_hook;
  std::function<std::optional<int64_t>(const std::string&, const std::vector<std::string>&)> execute_hook;
  std::function<void()> connect_hook;
  std::function<void()> close_hook;

  mutable int last_insert_id_count = 0;
  int64_t last_insert_id_value = 0;

  int connect_count = 0;
  int ping_count = 0;
  int close_count = 0;
  std::string last_sql;
  std::vector<std::string> last_params;

  bool connect(const DbConfig& /*config*/) override {
    ++connect_count;
    if (connect_hook) {
      connect_hook();
    }
    return connect_result;
  }

  bool is_open() const override { return is_open_result; }

  bool ping() override {
    ++ping_count;
    return ping_result;
  }

  std::optional<QueryResult> query(const std::string& sql, const std::vector<std::string>& params) override {
    last_sql = sql;
    last_params = params;
    if (query_hook) {
      return query_hook(sql, params);
    }
    return query_result;
  }

  std::optional<int64_t> execute(const std::string& sql, const std::vector<std::string>& params) override {
    last_sql = sql;
    last_params = params;
    if (execute_hook) {
      return execute_hook(sql, params);
    }
    return execute_result;
  }

  int64_t last_insert_id() const override {
    ++last_insert_id_count;
    return last_insert_id_value;
  }

  void close() override {
    ++close_count;
    if (close_hook) {
      close_hook();
    }
  }
};

} // namespace hps
