#include "logger.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <thread>
#include <vector>

using namespace hps;

// Logger 使用 std::call_once，整个进程生命周期只能 init 一次。
// 测试按顺序编排：先测未 init → init → init 后 → shutdown → shutdown 后。

TEST(LoggerTest, GetInstanceBeforeInitThrows) {
  Logger::shutdown();
  EXPECT_THROW(Logger::getInstance(), std::runtime_error);
}

TEST(LoggerTest, StaticApiBeforeInit) {
  Logger::shutdown();
  EXPECT_NO_THROW(Logger::_info("before"));
  EXPECT_NO_THROW(Logger::_error("before"));
  EXPECT_NO_THROW(Logger::_warn("before"));
  EXPECT_NO_THROW(Logger::_debug("before"));
}

// ------------- init 后 -------------

TEST(LoggerTest, InitShutdownLifecycle) {
  Logger::shutdown();
  Logger::init();
  EXPECT_NO_THROW(Logger::_info("test message"));
  // 不 shutdown 此处，留给后续测试继续用 init 状态
}

TEST(LoggerTest, InstanceAfterInit) {
  EXPECT_NO_THROW({
    auto& inst = Logger::getInstance();
    (void)inst;
  });
}

TEST(LoggerTest, DoubleInitSafe) {
  EXPECT_NO_THROW(Logger::init());
}

TEST(LoggerTest, MultipleThreadsLogging) {
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([]() {
      for (int j = 0; j < 10; ++j) {
        Logger::_info("multithreaded log");
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
}

// ------------- shutdown 后 -------------

TEST(LoggerTest, ShutdownTwiceSafe) {
  Logger::shutdown();
  EXPECT_NO_THROW(Logger::shutdown());
}

TEST(LoggerTest, StaticApiAfterShutdown) {
  EXPECT_NO_THROW(Logger::_info("after shutdown"));
}

TEST(LoggerTest, GetInstanceAfterShutdown) {
  EXPECT_THROW(Logger::getInstance(), std::runtime_error);
}

TEST(LoggerTest, InitShutdownCycle) {
  // call_once 已消耗，第二次 init 是 no-op，不抛异常
  EXPECT_NO_THROW(Logger::init());
  EXPECT_NO_THROW(Logger::shutdown());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
