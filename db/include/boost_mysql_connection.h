#pragma once

#include "iconnection.h"

namespace hps {

class BoostMySqlConnection : public IConnection {
public:
  BoostMySqlConnection();
  ~BoostMySqlConnection() override;

  BoostMySqlConnection(const BoostMySqlConnection&) = delete;
  BoostMySqlConnection& operator=(const BoostMySqlConnection&) = delete;
  BoostMySqlConnection(BoostMySqlConnection&&) = delete;
  BoostMySqlConnection& operator=(BoostMySqlConnection&&) = delete;

  bool connect(const DbConfig& config) override;
  bool is_open() const override;
  bool ping() override;
  std::optional<QueryResult> query(const std::string& sql, const std::vector<std::string>& params) override;
  std::optional<int64_t> execute(const std::string& sql, const std::vector<std::string>& params) override;
  void close() override;

private:
  class Impl;
  Impl* impl_;
};

} // namespace hps
