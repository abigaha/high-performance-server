#include "coroitem.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <vector>

using namespace hps;

TEST(CoroutineExtremeTest, CoroItemBasic) {
  auto make_coro = []() -> CoroItem<int> { co_return 42; };
  auto item = make_coro();
  EXPECT_FALSE(item.done());
  EXPECT_NO_THROW(item.resume());
  EXPECT_TRUE(item.done());
  EXPECT_TRUE(item.has_return_value());
  EXPECT_EQ(item.return_value(), 42);
}

TEST(CoroutineExtremeTest, CoroItemException) {
  auto make_coro = []() -> CoroItem<int> {
    throw std::runtime_error("coroutine error");
    co_return 0; // cppcheck-suppress unreachableCode
  };
  auto item = make_coro();
  EXPECT_FALSE(item.done());
  EXPECT_THROW(item.resume(), std::runtime_error);
  EXPECT_TRUE(item.done());
}

TEST(CoroutineExtremeTest, CoroItemVoid) {
  bool executed = false;
  auto make_coro = [&executed]() -> CoroItem<int> { // NOLINT(cppcoreguidelines-avoid-capturing-lambda-coroutines)
    executed = true;
    co_return 0;
  };
  auto item = make_coro();
  EXPECT_FALSE(item.done());
  item.resume();
  EXPECT_TRUE(item.done());
  EXPECT_TRUE(executed);
}

TEST(CoroutineExtremeTest, MultipleCoroutinesSequential) {
  for (int i = 0; i < 10; ++i) {
    auto make_coro = [i]() -> CoroItem<int> { // NOLINT(cppcoreguidelines-avoid-capturing-lambda-coroutines)
      co_return i * 2;
    };
    auto item = make_coro();
    item.resume();
    EXPECT_TRUE(item.done());
    EXPECT_EQ(item.return_value(), i * 2);
  }
}

TEST(CoroutineExtremeTest, CoroItemMoveOnly) {
  auto make_coro = []() -> CoroItem<std::unique_ptr<int>> { co_return std::make_unique<int>(42); };
  auto item = make_coro();
  item.resume();
  EXPECT_TRUE(item.done());
  EXPECT_TRUE(item.has_return_value());
  EXPECT_EQ(*item.return_value(), 42);
}

TEST(CoroutineExtremeTest, CoroItemLargeData) {
  auto make_coro = []() -> CoroItem<std::vector<char>> { co_return std::vector<char>(4096, 'L'); };
  auto item = make_coro();
  item.resume();
  EXPECT_TRUE(item.done());
  EXPECT_EQ(item.return_value().size(), static_cast<size_t>(4096));
  EXPECT_EQ(item.return_value()[0], 'L');
  EXPECT_EQ(item.return_value()[4095], 'L');
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
