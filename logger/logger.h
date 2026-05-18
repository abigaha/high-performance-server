#pragma once

#include <mutex>
#include <source_location>
#include <string>
#include <string_view>

#include <fstream>

namespace hps {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Logger {
public:
  explicit Logger(const std::string &file_path,
                  LogLevel min_level = LogLevel::Info);
  ~Logger();

  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;

  void set_level(LogLevel min_level);
  void
  debug(std::string_view message,
        const std::source_location location = std::source_location::current());
  void
  info(std::string_view message,
       const std::source_location location = std::source_location::current());
  void
  warn(std::string_view message,
       const std::source_location location = std::source_location::current());
  void
  error(std::string_view message,
        const std::source_location location = std::source_location::current());

private:
  void log(LogLevel level, std::string_view message,
           const std::source_location &location);
  static std::string level_to_string(LogLevel level);
  static std::string current_time_string();

  std::ofstream file_;
  std::mutex mutex_;
  LogLevel min_level_;
};

} // namespace hps
