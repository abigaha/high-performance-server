#include "boost_mysql_connection.h"

#include "detail/mysql_field_to_string.h"

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/mysql.hpp>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace hps {

namespace asio = boost::asio;
namespace mysql = boost::mysql;

namespace {

template <typename Value>
std::string floating_to_string(Value value) {
  std::array<char, 64> buffer{};
  const auto [end, error] = std::to_chars(buffer.begin(), buffer.end(), value);
  if (error != std::errc{}) {
    throw std::runtime_error("MySQL 浮点字段转换失败");
  }
  return std::string(buffer.data(), end);
}

template <typename Value>
std::string stream_to_string(const Value& value) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << value;
  return output.str();
}

std::string blob_to_string(mysql::blob_view value) {
  if (value.empty()) {
    return {};
  }
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string time_to_string(mysql::time value) {
  constexpr std::uint64_t kMicrosecondsPerSecond = 1'000'000;
  constexpr std::uint64_t kSecondsPerMinute = 60;
  constexpr std::uint64_t kMinutesPerHour = 60;

  const auto count = value.count();
  const bool is_negative = count < 0;
  const auto magnitude = is_negative ? static_cast<std::uint64_t>(-(count + 1)) + 1U
                                     : static_cast<std::uint64_t>(count);
  const auto total_seconds = magnitude / kMicrosecondsPerSecond;
  const auto microseconds = magnitude % kMicrosecondsPerSecond;
  const auto total_minutes = total_seconds / kSecondsPerMinute;
  const auto seconds = total_seconds % kSecondsPerMinute;
  const auto hours = total_minutes / kMinutesPerHour;
  const auto minutes = total_minutes % kMinutesPerHour;

  std::ostringstream output;
  output.imbue(std::locale::classic());
  if (is_negative) {
    output << '-';
  }
  output << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2) << minutes << ':' << std::setw(2)
         << seconds << '.' << std::setw(6) << microseconds;
  return output.str();
}

} // namespace

std::string detail::mysql_field_to_string(const mysql::field_view& field) {
  switch (field.kind()) {
    case mysql::field_kind::null:
      return {};
    case mysql::field_kind::int64:
      return std::to_string(field.as_int64());
    case mysql::field_kind::uint64:
      return std::to_string(field.as_uint64());
    case mysql::field_kind::string: {
      const auto value = field.as_string();
      return value.empty() ? std::string{} : std::string(value.data(), value.size());
    }
    case mysql::field_kind::blob:
      return blob_to_string(field.as_blob());
    case mysql::field_kind::float_:
      return floating_to_string(field.as_float());
    case mysql::field_kind::double_:
      return floating_to_string(field.as_double());
    case mysql::field_kind::date:
      return stream_to_string(field.as_date());
    case mysql::field_kind::datetime:
      return stream_to_string(field.as_datetime());
    case mysql::field_kind::time:
      return time_to_string(field.as_time());
  }
  throw std::logic_error("不支持的 MySQL 字段类型");
}

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
        std::transform(row.begin(), row.end(), std::back_inserter(row_str), [](const mysql::field_view& field) {
          return detail::mysql_field_to_string(field);
        });
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
    std::transform(params.begin(), params.end(), std::back_inserter(fvs), [](const std::string& param) {
      return mysql::field_view(mysql::string_view(param));
    });
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
