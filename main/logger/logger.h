#pragma once

#include <fstream>
#include <sstream>
// #include <mutex>
// #include <source_location>
#include <list>
#include <string>
// #include <string_view>
#include <stdint.h>

#include <memory>

namespace hps {

// 日志级别
enum class LogLevel { DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4, FATAL = 5 };

// 日志事件
class LogEvent {
public:
  using ptr = std::shared_ptr<LogEvent>;
  LogEvent();

private:
  const char* file_ = nullptr;  // 文件名
  int32_t line_ = 0;            // 行号
  uint32_t threadId_ = 0;       // 线程ID
  uint32_t fiberId_ = 0;        // 协程ID
  uint32_t elapse_ = 0;         // 程序启动开始到现在的毫秒数
  uint64_t time_ = 0;           // 时间戳
  std::string content_;         // 日志内容
};

// 日志格式器
class LogFormatter {
public:
  using ptr = std::shared_ptr<LogFormatter>;
  LogFormatter(const std::string& pattern);
  std::string format(LogLevel level, const LogEvent::ptr event);
};

// 日志输出地
class LogAppender {
public:
  using ptr = std::shared_ptr<LogAppender>;
  virtual ~LogAppender() = default;

  virtual void log(LogLevel level, const LogEvent::ptr event) = 0;

  void setFormatter(LogFormatter::ptr val) { formatter_ = val; }
  LogFormatter::ptr getFormatter() const { return formatter_; }

protected:
  LogLevel level_ = LogLevel::DEBUG;
  LogFormatter::ptr formatter_;
};

// 日志器
class Logger {
public:
  using ptr = std::shared_ptr<Logger>;
  void log(LogLevel level, const LogEvent::ptr event);
  Logger(const std::string& name = "root");

  void debug(const LogEvent::ptr event);
  void info(const LogEvent::ptr event);
  void warn(const LogEvent::ptr event);
  void error(const LogEvent::ptr event);
  void fatal(const LogEvent::ptr event);

  void addAppender(const LogAppender::ptr appender);
  void delAppender(const LogAppender::ptr appender);
  LogLevel getLevel() const { return level_; }
  void setLevel(LogLevel val) { level_ = val; }

private:
  LogLevel level_ = LogLevel::DEBUG;
  std::string name_;
  std::list<LogAppender::ptr> appenders_;
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
  FileLogAppender(const std::string& filename);
  void log(LogLevel level, const LogEvent::ptr event) override;

  bool reopen();

private:
  std::string filename_;
  std::ofstream filestream_;
};

}  // namespace hps
