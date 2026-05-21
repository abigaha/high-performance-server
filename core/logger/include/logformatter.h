#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "logger.h"

namespace hps {

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

  class CoroutineIdFormatItem : public FormatItem {
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

}  // namespace hps
