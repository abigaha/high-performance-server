#include "connection.h"
#include "coroitem.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <stdexcept>
#include <string_view>

using namespace hps;

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// T1: 协程异常传播——resume 后抛出，不 terminate（throw 后 co_return 不可达，仅为协程语法占位）
TEST(CoroutineTest, ExceptionPropagation) {
  auto make_coro = []() -> CoroItem<int> {
    throw std::runtime_error("test error");
    co_return 42; // cppcheck-suppress unreachableCode
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

// T3 (F6): co_await conn.await_read() 从对端读取数据
//   用 socketpair 构造一对互联 fd，向 sv[1] 写入数据，协程内 co_await 读 sv[0]
namespace {

sockaddr_in zero_addr() {
  sockaddr_in addr{};
  return addr;
}

CoroItem<int> read_coro(Connection& conn) {
  ssize_t n = co_await conn.await_read();
  co_return static_cast<int>(n);
}

CoroItem<int> write_coro(Connection& conn) {
  ssize_t n = co_await conn.await_write();
  co_return static_cast<int>(n);
}

} // namespace

TEST(CoroutineTest, AwaitReadReturnsBytes) {
  std::array<int, 2> sv{};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv.data()), 0);

  Connection conn(sv[0], zero_addr());

  // 向对端写入 5 字节
  ASSERT_EQ(::write(sv[1], "hello", 5), 5);

  auto item = read_coro(conn);
  EXPECT_FALSE(item.done());
  item.resume();
  EXPECT_TRUE(item.done());
  EXPECT_EQ(item.return_value(), 5);
  EXPECT_EQ(conn.read_buffer().size(), static_cast<std::size_t>(5));

  ::close(sv[1]);
}

TEST(CoroutineTest, AwaitReadOnEagainReturnsZero) {
  std::array<int, 2> sv{};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv.data()), 0);

  Connection conn(sv[0], zero_addr());

  // 不写入任何数据，read_from_fd 命中 EAGAIN 返回 0
  auto item = read_coro(conn);
  item.resume();
  EXPECT_TRUE(item.done());
  EXPECT_EQ(item.return_value(), 0);

  ::close(sv[1]);
}

TEST(CoroutineTest, AwaitWriteFlushesBuffer) {
  std::array<int, 2> sv{};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv.data()), 0);

  Connection conn(sv[0], zero_addr());

  // 预置写缓冲区数据，co_await await_write 刷出
  {
    std::lock_guard<std::mutex> lk(conn.write_mutex());
    conn.write_buffer().append("world");
  }

  auto item = write_coro(conn);
  item.resume();
  EXPECT_TRUE(item.done());
  EXPECT_GT(item.return_value(), 0);

  // 对端应收到数据
  std::array<char, 16> buf{};
  ssize_t n = ::read(sv[1], buf.data(), buf.size());
  ASSERT_GT(n, 0);
  EXPECT_EQ(std::string_view(buf.data(), static_cast<std::size_t>(n)), "world");

  ::close(sv[1]);
}
