#include "coroitem.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace hps;

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// T1: 协程异常传播——resume 后抛出，不 terminate
TEST(CoroutineTest, ExceptionPropagation) {
  auto make_coro = []() -> CoroItem<int> {
    throw std::runtime_error("test error");
    co_return 42;
  };

  auto item = make_coro();
  EXPECT_FALSE(item.done());
  EXPECT_THROW(item.resume(), std::runtime_error);
  EXPECT_TRUE(item.done());
}

// T2: 协程正常完成——无异常时正常返回
TEST(CoroutineTest, NormalCompletion) {
  auto make_coro = []() -> CoroItem<int> { co_return 42; };

  auto item = make_coro();
  EXPECT_FALSE(item.done());
  EXPECT_NO_THROW(item.resume());
  EXPECT_TRUE(item.done());
  EXPECT_TRUE(item.has_return_value());
  EXPECT_EQ(item.return_value(), 42);
}
