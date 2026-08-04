#pragma once

namespace hps::detail {

// MySQL statement handles require an explicit network close. Keep that cleanup
// coupled to the scope that prepared the statement, including exception paths.
template <typename Connection, typename Statement>
class PreparedStatementGuard {
public:
  PreparedStatementGuard(Connection& connection, const Statement& statement) :
      connection_(connection), statement_(statement) {}

  PreparedStatementGuard(const PreparedStatementGuard&) = delete;
  PreparedStatementGuard& operator=(const PreparedStatementGuard&) = delete;

  ~PreparedStatementGuard() {
    if (!closed_) {
      try {
        close();
      } catch (...) {
      }
    }
  }

  void close() {
    if (closed_) {
      return;
    }
    closed_ = true;
    connection_.close_statement(statement_);
  }

private:
  Connection& connection_;
  const Statement& statement_;
  bool closed_{false};
};

} // namespace hps::detail
