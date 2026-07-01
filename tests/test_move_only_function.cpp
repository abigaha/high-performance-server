#include "move_only_function.h"

#include <gtest/gtest.h>

#include <array>
#include <string>

using namespace hps;

namespace {
int g_call_count = 0;
} // namespace

// TM1: 小 lambda（<32字节）SBO 栈存储，调用正确
TEST(MoveOnlyFunctionTest, SmallLambda) {
  g_call_count = 0;
  int x = 10;
  MoveOnlyFunction f{[x]() { g_call_count = x; }};
  ASSERT_TRUE(static_cast<bool>(f));
  f();
  EXPECT_EQ(g_call_count, 10);
}

// TM2: 大 lambda（>32字节）堆存储，调用正确
TEST(MoveOnlyFunctionTest, LargeLambda) {
  g_call_count = 0;
  std::array<int, 16> large{};
  large.fill(42);
  MoveOnlyFunction f{[large]() { g_call_count = large[0]; }};
  ASSERT_TRUE(static_cast<bool>(f));
  f();
  EXPECT_EQ(g_call_count, 42);
}

// TM3: move 后原对象 empty，新对象可调用
TEST(MoveOnlyFunctionTest, MoveSemantics) {
  g_call_count = 0;
  int x = 99;
  MoveOnlyFunction f1{[x]() { g_call_count = x; }};
  MoveOnlyFunction f2{std::move(f1)};
  EXPECT_FALSE(static_cast<bool>(f1));
  ASSERT_TRUE(static_cast<bool>(f2));
  f2();
  EXPECT_EQ(g_call_count, 99);
}

// TM4: reset 后 operator bool 为 false
TEST(MoveOnlyFunctionTest, Reset) {
  MoveOnlyFunction f{[]() {}};
  ASSERT_TRUE(static_cast<bool>(f));
  f.reset();
  EXPECT_FALSE(static_cast<bool>(f));
}

// TM5: 空 MoveOnlyFunction 调用 operator() 不崩溃（no-op）
TEST(MoveOnlyFunctionTest, EmptyCall) {
  MoveOnlyFunction f;
  EXPECT_FALSE(static_cast<bool>(f));
  EXPECT_NO_THROW(f());
}

// TM6: LockFreeQueue 集成测试
#include "lock_free_queue.hpp"

TEST(MoveOnlyFunctionTest, LockFreeQueueIntegration) {
  g_call_count = 0;
  LockFreeQueue<MoveOnlyFunction> q;
  for (int i = 0; i < 10; ++i) {
    q.push(MoveOnlyFunction([i]() { g_call_count = i; }));
  }
  for (int i = 0; i < 10; ++i) {
    MoveOnlyFunction f;
    ASSERT_TRUE(q.try_pop(f));
    ASSERT_TRUE(static_cast<bool>(f));
    f();
    EXPECT_EQ(g_call_count, i);
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
