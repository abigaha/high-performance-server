#include "logger.h"

#include <chrono>
#include <ctime>
#include <iostream>
// #include <iomanip>
#include <functional>
#include <sstream>
#include <thread>

namespace hps {

// 程序启动时间
static auto g_start_time = std::chrono::high_resolution_clock::now();

LogEvent::LogEvent(const std::string& content, const std::source_location& loc)
    : file_(loc.file_name()), line_(loc.line()), content_(content) {
  // 获取线程ID
  auto id = std::this_thread::get_id();
  std::stringstream ss;
  ss << id;
  threadId_ = std::hash<std::string>{}(ss.str());

  // 获取当前时间戳
  auto now = std::chrono::system_clock::now();
  time_ = std::chrono::system_clock::to_time_t(now);

  // 获取程序运行时间
  auto elapsed = std::chrono::high_resolution_clock::now() - g_start_time;
  elapse_ = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

Logger::Logger(const std::string& name) : name_(name) {}

LogEvent::ptr Logger::createEvent(const std::string& content, const std::source_location& loc) {
  return std::make_shared<LogEvent>(content, loc);
}

void Logger::log(LogLevel level, const LogEvent::ptr event) {
  if (level >= level_) {
    for (const auto& it : appenders_) {
      it->log(level, event);
    }
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

void Logger::addAppender(const LogAppender::ptr appender) {
  if (appender) {
    appenders_.emplace_back(appender);
  }
}

void Logger::delAppender(const LogAppender::ptr appender) {
  for (auto it = appenders_.begin(); it != appenders_.end(); ++it) {
    if (*it == appender) {
      appenders_.erase(it);
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
  if (filestream_ && formatter_) {
    filestream_ << formatter_->format(level, event);
    filestream_.flush();
  }
}

void StdoutLogAppender::log(LogLevel level, const LogEvent::ptr event) {
  if (formatter_) {
    std::cout << formatter_->format(level, event);
  }
}

LogFormatter::LogFormatter(const std::string& pattern) : pattern_(pattern) {
  init();
}

std::string LogFormatter::format(LogLevel level, const LogEvent::ptr event) {
  std::stringstream ss;
  for (const auto& it : items_) {
    it->format(ss, level, event);
  }
  return ss.str();
}

void LogFormatter::init() {
  std::vector<std::tuple<std::string, std::string, int>> vec;
  std::string nstr;

  for (size_t i = 0; i < pattern_.size(); ++i) {
    if (pattern_[i] != '%') {
      nstr.append(1, pattern_[i]);
      continue;
    }

    if (nstr.size() > 0) {
      vec.emplace_back(nstr, "", 0);
      nstr.clear();
    }

    if (i + 1 >= pattern_.size()) {
      nstr.append(1, pattern_[i]);
      continue;
    }

    // 获取格式字符
    char fmt_char = pattern_[i + 1];
    if (!isalpha(fmt_char)) {
      nstr.append(1, pattern_[i]);
      continue;
    }

    i++;
    std::string fmt_param;

    // 检查是否有 {...} 参数
    if (i + 1 < pattern_.size() && pattern_[i + 1] == '{') {
      i++;
      size_t start = i + 1;
      while (i + 1 < pattern_.size() && pattern_[i + 1] != '}') {
        i++;
      }
      if (i + 1 < pattern_.size() && pattern_[i + 1] == '}') {
        fmt_param = pattern_.substr(start, i + 1 - start);
        i++;
      }
    }

    vec.emplace_back(std::string(1, fmt_char), fmt_param, 1);
  }

  if (nstr.size() > 0) {
    vec.emplace_back(nstr, "", 0);
  }

  for (const auto& it : vec) {
    if (std::get<2>(it) == 0) {
      items_.emplace_back(std::make_shared<StringFormatItem>(std::get<0>(it)));
    } else if (std::get<2>(it) == 1) {
      if (std::get<0>(it) == "m") {
        items_.emplace_back(std::make_shared<MessageFormatItem>());
      } else if (std::get<0>(it) == "p") {
        items_.emplace_back(std::make_shared<LevelFormatItem>());
      } else if (std::get<0>(it) == "r") {
        items_.emplace_back(std::make_shared<ElapseFormatItem>());
      } else if (std::get<0>(it) == "c") {
        items_.emplace_back(std::make_shared<NameFormatItem>());
      } else if (std::get<0>(it) == "t") {
        items_.emplace_back(std::make_shared<ThreadIdFormatItem>());
      } else if (std::get<0>(it) == "n") {
        items_.emplace_back(std::make_shared<NewLineFormatItem>());
      } else if (std::get<0>(it) == "d") {
        items_.emplace_back(std::make_shared<DateTimeFormatItem>(std::get<1>(it)));
      } else if (std::get<0>(it) == "f") {
        items_.emplace_back(std::make_shared<FileNameFormatItem>());
      } else if (std::get<0>(it) == "l") {
        items_.emplace_back(std::make_shared<LineFormatItem>());
      } else {
        items_.emplace_back(std::make_shared<PatternErrorFormatItem>());
      }
    }
  }
}

// FormatItem implementations

void LogFormatter::MessageFormatItem::format(std::ostream& os, [[maybe_unused]] LogLevel level,
                                             const LogEvent::ptr event) {
  os << event->getContent();
}

void LogFormatter::LevelFormatItem::format(std::ostream& os, LogLevel level,
                                           [[maybe_unused]] const LogEvent::ptr event) {
  switch (level) {
    case LogLevel::DEBUG:
      os << "DEBUG";
      break;
    case LogLevel::INFO:
      os << "INFO";
      break;
    case LogLevel::WARN:
      os << "WARN";
      break;
    case LogLevel::ERROR:
      os << "ERROR";
      break;
    case LogLevel::FATAL:
      os << "FATAL";
      break;
  }
}

void LogFormatter::ElapseFormatItem::format(std::ostream& os, [[maybe_unused]] LogLevel level,
                                            const LogEvent::ptr event) {
  os << event->getElapse();
}

void LogFormatter::NameFormatItem::format(std::ostream& os, [[maybe_unused]] LogLevel level,
                                          [[maybe_unused]] const LogEvent::ptr event) {
  os << "root";
}

void LogFormatter::ThreadIdFormatItem::format(std::ostream& os, [[maybe_unused]] LogLevel level,
                                              const LogEvent::ptr event) {
  os << event->getThreadId();
}

LogFormatter::DateTimeFormatItem::DateTimeFormatItem(const std::string& format) : format_(format) {}

void LogFormatter::DateTimeFormatItem::format(std::ostream& os, [[maybe_unused]] LogLevel level,
                                              const LogEvent::ptr event) {
  time_t time = event->getTime();
  struct tm* tm_info = localtime(&time);
  char buf[256] = {0};
  strftime(buf, sizeof(buf), format_.c_str(), tm_info);
  os << buf;
}

void LogFormatter::NewLineFormatItem::format(std::ostream& os, [[maybe_unused]] LogLevel level,
                                             [[maybe_unused]] const LogEvent::ptr event) {
  os << "\n";
}

void LogFormatter::StringFormatItem::format(std::ostream& os, [[maybe_unused]] LogLevel level,
                                            [[maybe_unused]] const LogEvent::ptr event) {
  os << str_;
}

void LogFormatter::LineFormatItem::format(std::ostream& os, [[maybe_unused]] LogLevel level,
                                          const LogEvent::ptr event) {
  os << event->getLine();
}

void LogFormatter::FileNameFormatItem::format(std::ostream& os, [[maybe_unused]] LogLevel level,
                                              const LogEvent::ptr event) {
  os << event->getFile();
}

void LogFormatter::PatternErrorFormatItem::format(std::ostream& os, [[maybe_unused]] LogLevel level,
                                                  [[maybe_unused]] const LogEvent::ptr event) {
  os << "<<pattern_error>>";
}

}  // namespace hps
