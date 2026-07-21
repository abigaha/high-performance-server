#include "logappender.h"
#include "logformatter.h"
#include "logger.h"
#include "qps_runner.hpp"

#include <cstdio>
#include <exception>
#include <memory>
#include <string>

namespace {

int run_benchmark() {
  auto levels = hps::bench::default_qps_levels();

  hps::bench::run_qps_steps("Logger Event Create", levels, [](int) {
    auto event = std::make_shared<hps::LogEvent>("qps benchmark log message", "qps_bench");
    return event->getContent() == "qps benchmark log message";
  });

  hps::bench::run_qps_steps("Logger Format", levels, [](int tid) {
    thread_local hps::LogFormatter formatter("%d{%Y-%m-%d %H:%M:%S} [%p] [%t] %c: %m%n");
    auto event =
      std::make_shared<hps::LogEvent>("thread=" + std::to_string(tid) + " action=benchmark latency=42ms", "qps_bench");
    return !formatter.format(hps::LogLevel::INFO, event).empty();
  });

  auto sink = std::make_shared<hps::FileLogAppender>("/dev/null");
  sink->set_auto_flush(false);
  sink->setFormatter(std::make_shared<hps::LogFormatter>("%d{%Y-%m-%d %H:%M:%S} [%p] [%t] %c: %m%n"));
  if (!sink->reopen()) {
    return 1;
  }
  hps::bench::run_qps_steps("Logger /dev/null Sink", levels, [&sink](int tid) {
    auto event = std::make_shared<hps::LogEvent>("thread=" + std::to_string(tid) + " sink=dev-null", "qps_bench");
    sink->log(hps::LogLevel::INFO, event);
    return true;
  });

  return 0;
}

} // namespace

int main() noexcept {
  try {
    return run_benchmark();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "日志 QPS 基准执行失败：%s\n", error.what());
  } catch (...) {
    std::fputs("日志 QPS 基准执行失败：未知异常\n", stderr);
  }
  return 1;
}
