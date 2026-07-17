#include "move_only_function.h"
#include "qps_runner.hpp"

int main() {
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

  return 0;
}
