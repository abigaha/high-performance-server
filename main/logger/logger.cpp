#include "logger.h"

#include <iostream>

namespace hps {

Logger::Logger(const std::string& name) : name_(name) {}

void Logger::log(LogLevel level, const LogEvent::ptr event) {
  if (level >= level_) {
    for (const auto& it : appenders_) {
      it->log(level, event);
    }
  }
}

void Logger::debug(const LogEvent::ptr event) {
  log(LogLevel::DEBUG, event);
}

void Logger::info(const LogEvent::ptr event) {
  log(LogLevel::INFO, event);
}

void Logger::warn(const LogEvent::ptr event) {
  log(LogLevel::WARN, event);
}

void Logger::error(const LogEvent::ptr event) {
  log(LogLevel::ERROR, event);
}

void Logger::fatal(const LogEvent::ptr event) {
  log(LogLevel::FATAL, event);
}

void Logger::addAppender(const LogAppender::ptr appender) {
  if (!appender)
    appenders_.emplace_back(appender);
}

void Logger::delAppender(const LogAppender::ptr appender) {
  for (const auto& it : appenders_) {
    if (it == appender) {
      appenders_.remove(it);
      break;
    }
  }
}

FileLogAppender::FileLogAppender(const std::string& filename) : filename_(filename) {
  reopen();
}

bool FileLogAppender::reopen() {
  if (filestream_) {
    filestream_.close();
  }
  filestream_.open(filename_, std::ios::app);
  return !!filestream_;
}

void FileLogAppender::log(LogLevel level, const LogEvent::ptr event) {
  if (filestream_) {
    filestream_ << formatter_->format(level, event);
  }
}

void StdoutLogAppender::log(LogLevel level, const LogEvent::ptr event) {
  std::cout << formatter_->format(level, event);
}

}  // namespace hps
