#include "connection.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <thread>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"

class ConnectionTest : public ::testing::Test {
protected:
  int sv_[2]{-1, -1};
  sockaddr_in addr_;

  void SetUp() override {
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_), 0) << "socketpair 创建失败";

    // 设置为非阻塞，避免 read_from_fd() 中的 ET 循环挂起
    for (int i = 0; i < 2; ++i) {
      int flags = fcntl(sv_[i], F_GETFL, 0);
      ASSERT_GE(flags, 0);
      ASSERT_EQ(fcntl(sv_[i], F_SETFL, flags | O_NONBLOCK), 0);
    }

    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr_.sin_port = htons(8080);
  }

  void TearDown() override {
    if (sv_[0] >= 0)
      close(sv_[0]);
    if (sv_[1] >= 0)
      close(sv_[1]);
  }
};

TEST_F(ConnectionTest, Construction) {
  hps::Connection conn(sv_[0], addr_);
  EXPECT_EQ(conn.fd(), sv_[0]);
  EXPECT_EQ(conn.client_port(), 8080);
  EXPECT_EQ(conn.state(), hps::Connection::State::CONNECTED);
}

TEST_F(ConnectionTest, InitialBuffersEmpty) {
  hps::Connection conn(sv_[0], addr_);
  EXPECT_TRUE(conn.read_buffer().empty());
  EXPECT_TRUE(conn.write_buffer().empty());
}

TEST_F(ConnectionTest, ReadFromFd) {
  hps::Connection conn(sv_[0], addr_);

  std::string test_data = "Hello, world!";
  (void)write(sv_[1], test_data.data(), test_data.size());
  // socketpair 保证写入成功

  // 给数据一点时间到达
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  ssize_t n = conn.read_from_fd();
  EXPECT_GT(n, 0);
  EXPECT_EQ(conn.read_buffer(), test_data);
}

TEST_F(ConnectionTest, ReadFromFdPartial) {
  hps::Connection conn(sv_[0], addr_);

  // 第一次写入部分数据
  (void)write(sv_[1], "Hello", 5);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ssize_t n1 = conn.read_from_fd();
  EXPECT_EQ(n1, 5);
  EXPECT_EQ(conn.read_buffer(), "Hello");

  // 第二次写入更多数据
  (void)write(sv_[1], ", world!", 8);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ssize_t n2 = conn.read_from_fd();
  EXPECT_EQ(n2, 8);
  EXPECT_EQ(conn.read_buffer(), "Hello, world!");
}

TEST_F(ConnectionTest, WriteToFd) {
  hps::Connection conn(sv_[0], addr_);

  conn.write_buffer() = "Response data";
  ssize_t n = conn.write_to_fd();
  EXPECT_EQ(n, 13);

  // 验证对端收到数据
  char buf[64];
  std::memset(buf, 0, sizeof(buf));
  ssize_t received = read(sv_[1], buf, sizeof(buf) - 1);
  EXPECT_EQ(received, 13);
  EXPECT_EQ(std::string(buf), "Response data");
}

TEST_F(ConnectionTest, ConsumeReadBuffer) {
  hps::Connection conn(sv_[0], addr_);

  // 先写入数据到读缓冲
  conn.read_buffer() = "abcdefghij";
  conn.consume_read_buffer(5);
  EXPECT_EQ(conn.read_buffer(), "fghij");

  // 消费全部
  conn.consume_read_buffer(5);
  EXPECT_TRUE(conn.read_buffer().empty());

  // 消费超过长度的数据
  conn.read_buffer() = "abc";
  conn.consume_read_buffer(100);
  EXPECT_TRUE(conn.read_buffer().empty());
}

TEST_F(ConnectionTest, CloseChangesState) {
  hps::Connection conn(sv_[0], addr_);
  EXPECT_EQ(conn.state(), hps::Connection::State::CONNECTED);

  conn.close();
  EXPECT_EQ(conn.state(), hps::Connection::State::CLOSED);
}

TEST_F(ConnectionTest, DoubleClose) {
  hps::Connection conn(sv_[0], addr_);
  conn.close();
  conn.close(); // 不应崩溃
  EXPECT_EQ(conn.state(), hps::Connection::State::CLOSED);
}

TEST_F(ConnectionTest, ReadAfterClose) {
  hps::Connection conn(sv_[0], addr_);
  conn.close();
  EXPECT_EQ(conn.read_from_fd(), -1);
}

TEST_F(ConnectionTest, WriteAfterClose) {
  hps::Connection conn(sv_[0], addr_);
  conn.write_buffer() = "data";
  conn.close();
  EXPECT_EQ(conn.write_to_fd(), -1);
}

#pragma GCC diagnostic pop

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
