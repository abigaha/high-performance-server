#include "boost_mysql_connection.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/mysql.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hps {

namespace asio = boost::asio;
namespace mysql = boost::mysql;

class BoostMySqlConnection::Impl {
public:
  Impl() : ssl_ctx_(asio::ssl::context::tls_client), conn_(ctx_.get_executor(), ssl_ctx_) {}

  bool connect(const DbConfig& config) {
    try {
      mysql::handshake_params params(config.username,
                                     config.password,
                                     config.database,
                                     mysql::handshake_params::default_collation,
                                     mysql::ssl_mode::require);

      asio::ip::tcp::resolver resolver(ctx_.get_executor());
      auto endpoints = resolver.resolve(config.host, std::to_string(config.port));

      mysql::tcp_ssl_connection temp(ctx_.get_executor(), ssl_ctx_);
      temp.connect(*endpoints.begin(), params);
      conn_ = std::move(temp);
      return true;
    } catch (const mysql::error_with_diagnostics& e) {
      std::cerr << "[mysql] connect error: " << e.what() << " | server: " << e.get_diagnostics().server_message()
                << std::endl;
      return false;
    } catch (const std::exception& e) {
      std::cerr << "[mysql] connect exception: " << e.what() << std::endl;
      return false;
    }
  }

  bool is_open() const { return conn_.stream().lowest_layer().is_open(); }

  bool ping() {
    try {
      mysql::results result;
      conn_.execute("SELECT 1", result);
      return !result.rows().empty();
    } catch (const mysql::error_with_diagnostics& e) {
      std::cerr << "[mysql] ping error: " << e.what() << " | server: " << e.get_diagnostics().server_message()
                << std::endl;
      return false;
    } catch (const std::exception& e) {
      std::cerr << "[mysql] ping exception: " << e.what() << std::endl;
      return false;
    }
  }

  std::optional<QueryResult> query(const std::string& sql, const std::vector<std::string>& params) {
    try {
      mysql::results result;
      if (params.empty()) {
        conn_.execute(sql, result);
      } else {
        auto stmt = conn_.prepare_statement(sql);
        execute_stmt(stmt, params, result);
      }

      last_id_ = static_cast<int64_t>(result.last_insert_id());

      QueryResult qr;
      for (const auto& col : result.meta()) {
        qr.columns.emplace_back(col.column_name());
      }
      for (const auto& row : result.rows()) {
        std::vector<std::string> row_str;
        row_str.reserve(row.size());
        for (const auto& fv : row) {
          if (fv.is_null()) {
            row_str.emplace_back();
          } else {
            row_str.emplace_back(fv.as_string());
          }
        }
        qr.rows.push_back(std::move(row_str));
      }
      return qr;
    } catch (const mysql::error_with_diagnostics& e) {
      std::cerr << "[mysql] query error: " << e.what() << " | server: " << e.get_diagnostics().server_message()
                << std::endl;
      return std::nullopt;
    } catch (const std::exception& e) {
      std::cerr << "[mysql] query exception: " << e.what() << std::endl;
      return std::nullopt;
    }
  }

  std::optional<int64_t> execute(const std::string& sql, const std::vector<std::string>& params) {
    try {
      mysql::results result;
      if (params.empty()) {
        conn_.execute(sql, result);
      } else {
        auto stmt = conn_.prepare_statement(sql);
        execute_stmt(stmt, params, result);
      }
      last_id_ = static_cast<int64_t>(result.last_insert_id());
      return static_cast<int64_t>(result.affected_rows());
    } catch (const mysql::error_with_diagnostics& e) {
      std::cerr << "[mysql] execute error: " << e.what() << " | server: " << e.get_diagnostics().server_message()
                << std::endl;
      return std::nullopt;
    } catch (const std::exception& e) {
      std::cerr << "[mysql] execute exception: " << e.what() << std::endl;
      return std::nullopt;
    }
  }

  int64_t last_insert_id() const { return last_id_; }

  void close_socket() {
    boost::system::error_code ec;
    if (conn_.stream().lowest_layer().is_open()) {
      // NOLINTNEXTLINE(bugprone-unused-return-value): asio 宏展开 false positive
      conn_.stream().lowest_layer().close(ec);
    }
  }

private:
  asio::io_context ctx_;
  asio::ssl::context ssl_ctx_;
  mysql::tcp_ssl_connection conn_;
  int64_t last_id_{0};

  void execute_stmt(const mysql::statement& stmt, const std::vector<std::string>& params, mysql::results& result) {
    // field_view 使用引用语义，执行期间参数字符串必须保持有效
    std::vector<mysql::field_view> fvs;
    fvs.reserve(params.size());
    for (const auto& p : params) {
      fvs.emplace_back(mysql::string_view(p));
    }
    conn_.execute(stmt.bind(fvs.begin(), fvs.end()), result);
  }
};

BoostMySqlConnection::BoostMySqlConnection() : impl_(new Impl()) {}

BoostMySqlConnection::~BoostMySqlConnection() {
  impl_->close_socket();
  delete impl_;
}

bool BoostMySqlConnection::connect(const DbConfig& config) {
  return impl_->connect(config);
}

bool BoostMySqlConnection::is_open() const {
  return impl_->is_open();
}

bool BoostMySqlConnection::ping() {
  return impl_->ping();
}

std::optional<QueryResult> BoostMySqlConnection::query(const std::string& sql, const std::vector<std::string>& params) {
  return impl_->query(sql, params);
}

std::optional<int64_t> BoostMySqlConnection::execute(const std::string& sql, const std::vector<std::string>& params) {
  return impl_->execute(sql, params);
}

int64_t BoostMySqlConnection::last_insert_id() const {
  return impl_->last_insert_id();
}

void BoostMySqlConnection::close() {
  impl_->close_socket();
}

} // namespace hps
