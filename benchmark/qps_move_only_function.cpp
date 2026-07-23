#include "move_only_function.h"
#include "qps_runner.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() noexcept {
  try {
    auto levels = hps::bench::default_qps_levels();

    // Construct and invoke
    {
      hps::bench::run_qps_steps("MoveOnlyFunction Construct+Invoke", levels, [](int) {
        int result = 0;
        hps::MoveOnlyFunction f([&result]() { result = 42; });
        f();
        (void)result;
      });
    }

    // Construct with large capture
    {
      struct BigObject {
        char data[256] = {};
        int value = 42;
      };

      hps::bench::run_qps_steps("MoveOnlyFunction Large Capture", levels, [](int) {
        BigObject obj;
        hps::MoveOnlyFunction f([obj]() { (void)obj.value; });
        f();
      });
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "仅移动函数 QPS 基准失败: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "仅移动函数 QPS 基准失败: 未知异常\n";
  }
  return EXIT_FAILURE;
}
