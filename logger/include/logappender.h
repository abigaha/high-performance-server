#pragma once

#include "logformatter.h"

#include <fstream>
#include <memory>
#include <mutex>

namespace hps {

// 日志输出地
class LogAppender {
public:
  using ptr = std::shared_ptr<LogAppender>;
  virtual ~LogAppender() = default;

  virtual void log(LogLevel level, const LogEvent::ptr event) = 0;

  void setFormatter(LogFormatter::ptr val) {
    std::lock_guard<std::mutex> lock(mutex_);
    formatter_ = val;
  }

  LogFormatter::ptr getFormatter() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return formatter_;
  }

protected:
  LogLevel level_ = LogLevel::DEBUG;
  LogFormatter::ptr formatter_;
  mutable std::mutex mutex_;
};

// 输出到控制台的Appender
class StdoutLogAppender : public LogAppender {
public:
  using ptr = std::shared_ptr<StdoutLogAppender>;
  void log(LogLevel level, const LogEvent::ptr event) override;
};

// 输出到文件的Appender
class FileLogAppender : public LogAppender {
public:
  using ptr = std::shared_ptr<FileLogAppender>;
  explicit FileLogAppender(const std::string& filename);
  void log(LogLevel level, const LogEvent::ptr event) override;

  bool reopen();

private:
  std::string filename_;
  std::ofstream filestream_;
};

} // namespace hps
