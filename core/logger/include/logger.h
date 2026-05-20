#pragma once

#include <stdint.h>

#include <fstream>
#include <list>
#include <memory>
#include <source_location>
#include <string>
#include <vector>

namespace hps {

// 日志级别
enum class LogLevel { DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4, FATAL = 5 };

// 日志事件
class LogEvent {
public:
  using ptr = std::shared_ptr<LogEvent>;
  LogEvent(const std::string& content,
           const std::source_location& loc = std::source_location::current());

  const char* getFile() const { return file_; }
  int32_t getLine() const { return line_; }
  uint32_t getThreadId() const { return threadId_; }
  uint32_t getElapse() const { return elapse_; }
  uint64_t getTime() const { return time_; }
  const std::string& getContent() const { return content_; }

private:
  const char* file_ = nullptr;  // 文件名
  int32_t line_ = 0;            // 行号
  uint32_t threadId_ = 0;       // 线程ID
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

private:
  class FormatItem {
  public:
    using ptr = std::shared_ptr<FormatItem>;
    virtual ~FormatItem() = default;
    virtual void format(std::ostream& os, LogLevel level, const LogEvent::ptr event) = 0;
  };

  class MessageFormatItem : public FormatItem {
  public:
    void format(std::ostream& os, LogLevel level, const LogEvent::ptr event) override;
  };

  class LevelFormatItem : public FormatItem {
  public:
    void format(std::ostream& os, LogLevel level, const LogEvent::ptr event) override;
  };

  class ElapseFormatItem : public FormatItem {
  public:
    void format(std::ostream& os, LogLevel level, const LogEvent::ptr event) override;
  };

  class NameFormatItem : public FormatItem {
  public:
    void format(std::ostream& os, LogLevel level, const LogEvent::ptr event) override;
  };

  class ThreadIdFormatItem : public FormatItem {
  public:
    void format(std::ostream& os, LogLevel level, const LogEvent::ptr event) override;
  };

  class DateTimeFormatItem : public FormatItem {
  public:
    DateTimeFormatItem(const std::string& format = "%Y-%m-%d %H:%M:%S");
    void format(std::ostream& os, LogLevel level, const LogEvent::ptr event) override;

  private:
    std::string format_;
  };

  class NewLineFormatItem : public FormatItem {
  public:
    void format(std::ostream& os, LogLevel level, const LogEvent::ptr event) override;
  };

  class StringFormatItem : public FormatItem {
  public:
    StringFormatItem(const std::string& str) : str_(str) {}
    void format(std::ostream& os, LogLevel level, const LogEvent::ptr event) override;

  private:
    std::string str_;
  };

  class LineFormatItem : public FormatItem {
  public:
    void format(std::ostream& os, LogLevel level, const LogEvent::ptr event) override;
  };

  class FileNameFormatItem : public FormatItem {
  public:
    void format(std::ostream& os, LogLevel level, const LogEvent::ptr event) override;
  };

  class PatternErrorFormatItem : public FormatItem {
  public:
    void format(std::ostream& os, LogLevel level, const LogEvent::ptr event) override;
  };

  void init();

private:
  std::string pattern_;
  std::vector<FormatItem::ptr> items_;
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
  Logger(const std::string& name = "root");

  void log(LogLevel level, const LogEvent::ptr event);

  LogEvent::ptr createEvent(const std::string& content,
                            const std::source_location& loc = std::source_location::current());

  void debug(const std::string& msg,
             const std::source_location& loc = std::source_location::current());
  void info(const std::string& msg,
            const std::source_location& loc = std::source_location::current());
  void warn(const std::string& msg,
            const std::source_location& loc = std::source_location::current());
  void error(const std::string& msg,
             const std::source_location& loc = std::source_location::current());
  void fatal(const std::string& msg,
             const std::source_location& loc = std::source_location::current());

  void addAppender(const LogAppender::ptr appender);
  void delAppender(const LogAppender::ptr appender);
  LogLevel getLevel() const { return level_; }
  void setLevel(LogLevel val) { level_ = val; }

  Logger& operator<<(const std::string& msg) {
    log(level_, createEvent(msg));
    return *this;
  }

  static Logger& getInstance();

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
