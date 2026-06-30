#include "logformatter.h"

#include <array>
#include <cctype>
#include <ctime>
#include <sstream>
#include <tuple>

namespace hps {

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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void LogFormatter::init() {
  std::vector<std::tuple<std::string, std::string, int>> vec;
  std::string nstr;

  for (size_t i = 0; i < pattern_.size(); ++i) {
    if (pattern_[i] != '%') {
      nstr.append(1, pattern_[i]);
      continue;
    }

    if (!nstr.empty()) {
      vec.emplace_back(nstr, "", 0);
      nstr.clear();
    }

    if (i + 1 >= pattern_.size()) {
      nstr.append(1, pattern_[i]);
      continue;
    }

    // 获取格式字符
    char fmt_char = pattern_[i + 1];
    if (isalpha(static_cast<unsigned char>(fmt_char)) == 0) {
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

  if (!nstr.empty()) {
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
      } else if (std::get<0>(it) == "C") {
        items_.emplace_back(std::make_shared<CoroutineIdFormatItem>());
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

void LogFormatter::MessageFormatItem::format(std::ostream& os,
                                             [[maybe_unused]] LogLevel level,
                                             const LogEvent::ptr event) {
  os << event->getContent();
}

void LogFormatter::LevelFormatItem::format(std::ostream& os,
                                           LogLevel level,
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

void LogFormatter::ElapseFormatItem::format(std::ostream& os,
                                            [[maybe_unused]] LogLevel level,
                                            const LogEvent::ptr event) {
  os << event->getElapse();
}

void LogFormatter::NameFormatItem::format(std::ostream& os,
                                          [[maybe_unused]] LogLevel level,
                                          const LogEvent::ptr event) {
  os << event->get_logger_name();
}

void LogFormatter::ThreadIdFormatItem::format(std::ostream& os,
                                              [[maybe_unused]] LogLevel level,
                                              const LogEvent::ptr event) {
  os << event->get_thread_id();
}

void LogFormatter::CoroutineIdFormatItem::format(std::ostream& os,
                                                 [[maybe_unused]] LogLevel level,
                                                 const LogEvent::ptr event) {
  os << event->get_coroutine_id();
}

LogFormatter::DateTimeFormatItem::DateTimeFormatItem(const std::string& format) : format_(format) {}

void LogFormatter::DateTimeFormatItem::format(std::ostream& os,
                                              [[maybe_unused]] LogLevel level,
                                              const LogEvent::ptr event) {
  auto time = static_cast<time_t>(event->getTime());
  struct tm tm_info;
  localtime_r(&time, &tm_info);
  std::array<char, 256> buf{};
  strftime(buf.data(), buf.size(), format_.c_str(), &tm_info);
  os << buf.data();
}

void LogFormatter::NewLineFormatItem::format(std::ostream& os,
                                             [[maybe_unused]] LogLevel level,
                                             [[maybe_unused]] const LogEvent::ptr event) {
  os << "\n";
}

void LogFormatter::StringFormatItem::format(std::ostream& os,
                                            [[maybe_unused]] LogLevel level,
                                            [[maybe_unused]] const LogEvent::ptr event) {
  os << str_;
}

void LogFormatter::LineFormatItem::format(std::ostream& os,
                                          [[maybe_unused]] LogLevel level,
                                          const LogEvent::ptr event) {
  os << event->getLine();
}

void LogFormatter::FileNameFormatItem::format(std::ostream& os,
                                              [[maybe_unused]] LogLevel level,
                                              const LogEvent::ptr event) {
  os << event->getFile();
}

void LogFormatter::PatternErrorFormatItem::format(std::ostream& os,
                                                  [[maybe_unused]] LogLevel level,
                                                  [[maybe_unused]] const LogEvent::ptr event) {
  os << "<<pattern_error>>";
}

} // namespace hps
