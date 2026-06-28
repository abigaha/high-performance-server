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
    filestream_.flush();
  }
}

void StdoutLogAppender::log(LogLevel level, const LogEvent::ptr event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (formatter_) {
    std::cout << formatter_->format(level, event);
  }
}

} // namespace hps
