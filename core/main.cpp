#include "logappender.h"
#include "logger.h"

int main() {
  // 创建 Logger
  auto logger = std::make_shared<hps::Logger>("server");

  auto& loggerInstance = hps::Logger::getInstance();

  // 创建文件 Appender
  auto file = std::make_shared<hps::FileLogAppender>("server.log");
  auto file_fmt = std::make_shared<hps::LogFormatter>("%d{%Y-%m-%d %H:%M:%S} [%p] [%t] %f:%l %m%n");
  file->setFormatter(file_fmt);
  logger->addAppender(file);

  // 设置日志级别
  logger->setLevel(hps::LogLevel::INFO);

  // 记录日志
  logger->debug("Debug info (skipped)");
  logger->info("Server starting...");
  logger->warn("Memory usage at 80%");
  logger->error("Failed to connect to database");

  loggerInstance.debug("Debug info (skipped)");
  loggerInstance.info("Server starting...");
  loggerInstance.warn("Memory usage at 80%");
  loggerInstance.error("Failed to connect to database");

  return 0;
}
