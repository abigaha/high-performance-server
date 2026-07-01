#include "coroitem.hpp"
#include "logappender.h"
#include "logger.h"

hps::CoroItem<int> exampleCoroutine() {
  hps::Logger::_info("Coroutine started");
  co_return 42;
}

int main() {
  // 两阶段生命周期：init → 使用 → shutdown
  hps::Logger::init("server");
  auto& logger = hps::Logger::getInstance();

  // 创建文件 Appender
  auto file = std::make_shared<hps::FileLogAppender>("server.log");
  auto file_fmt = std::make_shared<hps::LogFormatter>("%d{%Y-%m-%d %H:%M:%S} [%p] [%t] %f:%l %m%n");
  file->setFormatter(file_fmt);
  logger.addAppender(file);

  // 设置日志级别
  logger.setLevel(hps::LogLevel::INFO);

  // 记录日志
  logger.debug("Debug info (skipped)");
  logger.info("Server starting...");
  logger.warn("Memory usage at 80%");
  logger.error("Failed to connect to database");

  // 测试协程
  auto coro = exampleCoroutine();
  coro.resume();
  if (coro.done()) {
    std::cout << "Coroutine returned value: " << coro.return_value() << std::endl;
  }

  hps::Logger::shutdown();
  return 0;
}
