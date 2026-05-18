#pragma once

// #include <fstream>
// #include <mutex>
// #include <source_location>
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
  typedef std::shared_ptr<LogEvent> ptr;
  LogEvent();

private:
  const char* m_file = nullptr;  // 文件名
  int32_t m_line = 0;            // 行号
  uint32_t m_threadId = 0;       // 线程ID
  uint32_t m_fiberId = 0;        // 协程ID
  uint32_t m_elapse = 0;         // 程序启动开始到现在的毫秒数
  uint64_t m_time = 0;           // 时间戳
  std::string m_content;         // 日志内容
};

// 日志格式器
class LogFormatter {
public:
  typedef std::shared_ptr<LogFormatter> ptr;
  LogFormatter(const std::string& pattern);
  std::string format(LogLevel level, const LogEvent::ptr event);
};

// 日志输出地
class LogAppender {
public:
  typedef std::shared_ptr<LogAppender> ptr;
  virtual ~LogAppender() = default;

  void log(LogLevel level, const LogEvent::ptr event);

private:
  LogLevel m_level = LogLevel::DEBUG;
};

// 日志器
class Logger {
public:
  typedef std::shared_ptr<Logger> ptr;
  void log(LogLevel level, const LogEvent::ptr event);
  Logger(const std::string& name = "root");

private:
  LogLevel level = LogLevel::DEBUG;
  std::string m_name;
  LogAppender::ptr m_appender;
};

// 输出到控制台的Appender
class StdoutLogAppender : public LogAppender {};

// 输出到文件的Appender
class FileLogAppender : public LogAppender {};

}  // namespace hps
