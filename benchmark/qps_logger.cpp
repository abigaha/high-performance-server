#include "logger.h"
#include "qps_runner.hpp"

#include <string>

int main() {
  hps::Logger::init("qps_bench");
  auto levels = hps::bench::default_qps_levels();

  hps::bench::run_qps_steps("Logger Info", levels, [](int) { hps::Logger::_info("qps benchmark log message"); });

  hps::bench::run_qps_steps("Logger Error", levels, [](int) { hps::Logger::_error("qps benchmark error message"); });

  hps::bench::run_qps_steps("Logger Format", levels, [](int tid) {
    auto msg = "thread=" + std::to_string(tid) + " action=benchmark latency=42ms";
    hps::Logger::_info(msg);
  });

  hps::Logger::shutdown();
  return 0;
}
