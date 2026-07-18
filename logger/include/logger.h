#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>

namespace hps {

class LogAppender;

// 日志级别
enum class LogLevel { DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4, FATAL = 5 };

// 日志事件
class LogEvent {
public:
  using ptr = std::shared_ptr<LogEvent>;
  explicit LogEvent(const std::string& content,
                    const std::string& logger_name = "root",
                    const std::source_location& loc = std::source_location::current());

  const char* getFile() const { return file_; }

  int32_t getLine() const { return line_; }

  uint32_t get_thread_id() const { return thread_id_; }

  uint32_t getElapse() const { return elapse_; }

  uint64_t getTime() const { return time_; }

  uint64_t get_coroutine_id() const { return coroutine_id_; }

  const std::string& get_logger_name() const { return logger_name_; }

  const std::string& getContent() const { return content_; }

private:
  const char* file_ = nullptr; // 文件名
  int32_t line_ = 0;           // 行号
  uint32_t thread_id_ = 0;     // 线程ID
  uint32_t elapse_ = 0;        // 程序启动开始到现在的毫秒数
  uint64_t time_ = 0;          // 时间戳
  uint64_t coroutine_id_ = 0;  // 协程ID
  std::string logger_name_;    // 日志器名称
  std::string content_;        // 日志内容
};

// 日志器
class Logger {
public:
  using ptr = std::shared_ptr<Logger>;

  void log(LogLevel level, const LogEvent::ptr event);

  LogEvent::ptr createEvent(const std::string& content,
                            const std::source_location& loc = std::source_location::current());

  class CoroutineScope {
  public:
    explicit CoroutineScope(uint64_t id);
    ~CoroutineScope();

    CoroutineScope(const CoroutineScope&) = delete;
    CoroutineScope& operator=(const CoroutineScope&) = delete;

  private:
    uint64_t prev_ = 0;
  };

  static uint64_t allocCoroutineId();
  static void setCurrentCoroutineId(uint64_t id);
  static uint64_t getCurrentCoroutineId();

  void debug(const std::string& msg, const std::source_location& loc = std::source_location::current());
  void info(const std::string& msg, const std::source_location& loc = std::source_location::current());
  void warn(const std::string& msg, const std::source_location& loc = std::source_location::current());
  void error(const std::string& msg, const std::source_location& loc = std::source_location::current());
  void fatal(const std::string& msg, const std::source_location& loc = std::source_location::current());

  void addAppender(const std::shared_ptr<LogAppender>& appender);
  void delAppender(const std::shared_ptr<LogAppender>& appender);

  LogLevel getLevel() const { return level_.load(std::memory_order_relaxed); }

  void setLevel(LogLevel val) { level_.store(val, std::memory_order_relaxed); }

  // N12-L：移除语义混乱的 operator<<（用 getLevel() 作日志级别），改用显式 debug/info/... 方法

  // 两阶段生命周期管理
  static void init(const std::string& name = "server");
  static void shutdown();
  static Logger& getInstance();

  // 静态便捷 API：未 init 时静默丢弃（测试兼容）；直接 getInstance() 调用强限制抛异常
  static void _debug(const std::string& msg, const std::source_location& loc = std::source_location::current()) {
    if (s_instance_) {
      s_instance_->debug(msg, loc);
    }
  }

  static void _info(const std::string& msg, const std::source_location& loc = std::source_location::current()) {
    if (s_instance_) {
      s_instance_->info(msg, loc);
    }
  }

  static void _warn(const std::string& msg, const std::source_location& loc = std::source_location::current()) {
    if (s_instance_) {
      s_instance_->warn(msg, loc);
    }
  }

  static void _error(const std::string& msg, const std::source_location& loc = std::source_location::current()) {
    if (s_instance_) {
      s_instance_->error(msg, loc);
    }
  }

  static void _fatal(const std::string& msg, const std::source_location& loc = std::source_location::current()) {
    if (s_instance_) {
      s_instance_->fatal(msg, loc);
    }
  }

private:
  // 构造函数私有，仅允许 init() 通过静态成员权限构造，保证单例不被破坏
  explicit Logger(const std::string& name = "root");

  std::atomic<LogLevel> level_{LogLevel::DEBUG};
  std::string name_;
  std::list<std::shared_ptr<LogAppender>> appenders_;
  mutable std::mutex mutex_;

  static std::unique_ptr<Logger> s_instance_;
  static std::once_flag s_init_flag_;
};

} // namespace hps
