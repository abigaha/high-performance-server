#include "logappender.h"

#include <iostream>

namespace hps {

FileLogAppender::FileLogAppender(const std::string& filename) : filename_(filename) {
  reopen();
}

bool FileLogAppender::reopen() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (filestream_) {
    filestream_.close();
  }
  filestream_.open(filename_, std::ios::app);
  return !!filestream_;
}

void FileLogAppender::log(LogLevel level, const LogEvent::ptr event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (filestream_ && formatter_) {
    filestream_ << formatter_->format(level, event);
    // L4-L：可配置 auto_flush，默认 true 保持 crash-safe；高频场景可设 false 提性能
    if (auto_flush_) {
      filestream_.flush();
    }
  }
}

void StdoutLogAppender::log(LogLevel level, const LogEvent::ptr event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (formatter_) {
    std::cout << formatter_->format(level, event);
  }
}

} // namespace hps
