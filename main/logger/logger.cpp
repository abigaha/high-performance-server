#include "logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace hps {

Logger::Logger(const std::string& file_path, LogLevel min_level)
    : file_(file_path, std::ios::app), min_level_(min_level) {
  if (!file_.is_open()) {
    throw std::runtime_error("failed to open log file: " + file_path);
  }
}

Logger::~Logger() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_.is_open()) {
    file_.flush();
  }
}

void Logger::set_level(LogLevel min_level) {
  std::lock_guard<std::mutex> lock(mutex_);
  min_level_ = min_level;
}

void Logger::debug(std::string_view message, const std::source_location location) {
  log(LogLevel::Debug, message, location);
}

void Logger::info(std::string_view message, const std::source_location location) {
  log(LogLevel::Info, message, location);
}

void Logger::warn(std::string_view message, const std::source_location location) {
  log(LogLevel::Warn, message, location);
}

void Logger::error(std::string_view message, const std::source_location location) {
  log(LogLevel::Error, message, location);
}

void Logger::log(LogLevel level, std::string_view message, const std::source_location& location) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (static_cast<int>(level) < static_cast<int>(min_level_)) {
    return;
  }

  std::ostringstream thread_id_stream;
  thread_id_stream << std::this_thread::get_id();

  file_ << current_time_string() << " [" << level_to_string(level) << "] "
        << "[tid:" << thread_id_stream.str() << "] " << location.file_name() << ":"
        << location.line() << " " << message << '\n';
  file_.flush();
}

std::string Logger::level_to_string(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warn:
      return "WARN";
    case LogLevel::Error:
      return "ERROR";
    default:
      return "UNKNOWN";
  }
}

std::string Logger::current_time_string() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

  std::tm local_time{};
#if defined(_WIN32)
  localtime_s(&local_time, &now_time);
#else
  localtime_r(&now_time, &local_time);
#endif

  std::ostringstream stream;
  stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
  return stream.str();
}

}  // namespace hps
