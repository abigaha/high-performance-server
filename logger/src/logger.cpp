#include "logger.h"

#include "logappender.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <sstream>
#include <thread>
#include <vector>

namespace {

const auto g_start_time = std::chrono::high_resolution_clock::now();
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<uint64_t> g_coroutine_id_seed{1};
thread_local uint64_t g_current_coroutine_id = 0;
// N6-M：thread_local 缓存 tid，避免每条日志 stringstream+hash 开销
thread_local uint32_t g_cached_tid = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

} // anonymous namespace

namespace hps {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
LogEvent::LogEvent(const std::string& content, const std::string& logger_name, const std::source_location& loc) :
    file_(loc.file_name()),
    line_(static_cast<int32_t>(loc.line())),
    coroutine_id_(Logger::getCurrentCoroutineId()),
    logger_name_(logger_name),
    content_(content) {
  // N6-M：tid 使用 thread_local 缓存，仅首次计算
  if (g_cached_tid == 0) {
    auto id = std::this_thread::get_id();
    std::stringstream ss;
    ss << id;
    g_cached_tid = static_cast<uint32_t>(std::hash<std::string>{}(ss.str()));
  }
  thread_id_ = g_cached_tid;

  // 获取当前时间戳
  auto now = std::chrono::system_clock::now();
  time_ = std::chrono::system_clock::to_time_t(now);

  // 获取程序运行时间
  auto elapsed = std::chrono::high_resolution_clock::now() - g_start_time;
  elapse_ = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

Logger::Logger(const std::string& name) : name_(name) {}

LogEvent::ptr Logger::createEvent(const std::string& content, const std::source_location& loc) {
  return std::make_shared<LogEvent>(content, name_, loc);
}

uint64_t Logger::allocCoroutineId() {
  return g_coroutine_id_seed.fetch_add(1, std::memory_order_relaxed);
}

void Logger::setCurrentCoroutineId(uint64_t id) {
  g_current_coroutine_id = id;
}

uint64_t Logger::getCurrentCoroutineId() {
  return g_current_coroutine_id;
}

Logger::CoroutineScope::CoroutineScope(uint64_t id) : prev_(Logger::getCurrentCoroutineId()) {
  Logger::setCurrentCoroutineId(id);
}

Logger::CoroutineScope::~CoroutineScope() {
  Logger::setCurrentCoroutineId(prev_);
}

void Logger::log(LogLevel level, const LogEvent::ptr event) {
  if (level < level_.load(std::memory_order_relaxed)) {
    return;
  }
  std::vector<std::shared_ptr<LogAppender>> appenders;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    appenders.assign(appenders_.begin(), appenders_.end());
  }
  for (const auto& it : appenders) {
    it->log(level, event);
  }
}

void Logger::debug(const std::string& msg, const std::source_location& loc) {
  log(LogLevel::DEBUG, createEvent(msg, loc));
}

void Logger::info(const std::string& msg, const std::source_location& loc) {
  log(LogLevel::INFO, createEvent(msg, loc));
}

void Logger::warn(const std::string& msg, const std::source_location& loc) {
  log(LogLevel::WARN, createEvent(msg, loc));
}

void Logger::error(const std::string& msg, const std::source_location& loc) {
  log(LogLevel::ERROR, createEvent(msg, loc));
}

void Logger::fatal(const std::string& msg, const std::source_location& loc) {
  log(LogLevel::FATAL, createEvent(msg, loc));
}

void Logger::addAppender(const std::shared_ptr<LogAppender>& appender) {
  if (!appender) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  appenders_.emplace_back(appender);
}

void Logger::delAppender(const std::shared_ptr<LogAppender>& appender) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find(appenders_.begin(), appenders_.end(), appender);
  if (it != appenders_.end()) {
    appenders_.erase(it);
  }
}

Logger& Logger::getInstance() {
  // N11-L：magic statics 已线程安全，移除冗余 call_once；
  //        不硬编码 FileLogAppender（避免与用户配置重复写文件），仅默认 stdout
  static Logger instance("server");
  static std::once_flag once;
  std::call_once(once, []() {
    auto appender = std::make_shared<StdoutLogAppender>();
    auto formatter = std::make_shared<LogFormatter>("%d{%Y-%m-%d %H:%M:%S} [%p] [%t] [%C] %c: %f%l : %m%n");
    appender->setFormatter(formatter);
    instance.addAppender(appender);
    instance.setLevel(LogLevel::DEBUG);
  });
  return instance;
}

} // namespace hps
