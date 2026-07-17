#include "tcp_client.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

namespace {

int create_echo_server() {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr {};

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 5) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int get_port(int fd) {
  struct sockaddr_in addr {};

  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
    return -1;
  }
  return ntohs(addr.sin_port);
}

void run_echo_server(int listen_fd) {
  int client = accept(listen_fd, nullptr, nullptr);
  if (client < 0)
    return;

  std::array<char, 4096> buf{};
  ssize_t n = 0;
  while ((n = recv(client, buf.data(), buf.size(), 0)) > 0) {
    send(client, buf.data(), static_cast<std::size_t>(n), 0);
  }
  close(client);
}

std::string temp_file_path() {
  const char* tmpdir = std::getenv("TMPDIR");
  if (tmpdir == nullptr)
    tmpdir = "/tmp";
  return std::string(tmpdir) + "/opencode_test_tcp_client_" + std::to_string(getpid());
}

// 持续读取直到获得期望字节数
bool read_until_size(hps::TcpClient& client, std::string& data, std::size_t expected_size) {
  data.clear();
  while (data.size() < expected_size) {
    std::string chunk;
    if (!client.receive_message(chunk, hps::ReadMode::RAW, 5000)) {
      return data.size() == expected_size;
    }
    data += chunk;
  }
  return true;
}

} // anonymous namespace

TEST(TcpClientTest, ConnectTimeout) {
  hps::TcpClient client(100, "127.0.0.1", 1);
  EXPECT_FALSE(client.connect_to_server());
  EXPECT_FALSE(client.is_connected());
}

TEST(TcpClientTest, SendReceiveLine) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::thread t(run_echo_server, srv);

  hps::TcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connect_to_server());
  EXPECT_TRUE(client.is_connected());

  EXPECT_TRUE(client.send_message("hello\n"));
  std::string reply;
  EXPECT_TRUE(client.receive_message(reply, hps::ReadMode::LINE));
  EXPECT_EQ(reply, "hello");

  client.disconnect();
  t.join();
  close(srv);
}

TEST(TcpClientTest, ReceiveTimeout) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::thread t(run_echo_server, srv);

  hps::TcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connect_to_server());

  std::string reply;
  EXPECT_FALSE(client.receive_message(reply, hps::ReadMode::RAW, 100));

  client.disconnect();
  t.join();
  close(srv);
}

TEST(TcpClientTest, SendFile) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::thread t(run_echo_server, srv);

  std::string path = temp_file_path();
  {
    std::ofstream f(path, std::ios::binary);
    ASSERT_TRUE(f);
    f << "file_content_123";
  }

  hps::TcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connect_to_server());
  EXPECT_TRUE(client.send_file(path));

  std::string reply;
  EXPECT_TRUE(read_until_size(client, reply, std::string("file_content_123").size()));
  EXPECT_EQ(reply, "file_content_123");

  client.disconnect();
  t.join();
  close(srv);
  std::remove(path.c_str());
}

TEST(TcpClientTest, SendLargeData) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::string large_data(10000, 'x');

  std::thread t(run_echo_server, srv);

  hps::TcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connect_to_server());
  EXPECT_TRUE(client.send_file(large_data, large_data.size()));

  std::string reply;
  EXPECT_TRUE(read_until_size(client, reply, large_data.size()));
  EXPECT_EQ(reply.size(), large_data.size());

  client.disconnect();
  t.join();
  close(srv);
}

TEST(TcpClientTest, DisconnectReconnect) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  // 第一次连接
  std::thread t1(run_echo_server, srv);
  {
    hps::TcpClient client("127.0.0.1", static_cast<uint16_t>(port));
    ASSERT_TRUE(client.connect_to_server());
    EXPECT_TRUE(client.is_connected());
    EXPECT_TRUE(client.send_message("first\n"));
    std::string reply;
    EXPECT_TRUE(client.receive_message(reply, hps::ReadMode::LINE));
    EXPECT_EQ(reply, "first");
    client.disconnect();
    EXPECT_FALSE(client.is_connected());
  }
  t1.join();

  // 重连（创建新 server 线程，前一个已退出）
  std::thread t2(run_echo_server, srv);
  {
    hps::TcpClient client("127.0.0.1", static_cast<uint16_t>(port));
    ASSERT_TRUE(client.connect_to_server());
    EXPECT_TRUE(client.is_connected());
    EXPECT_TRUE(client.send_message("second\n"));
    std::string reply;
    EXPECT_TRUE(client.receive_message(reply, hps::ReadMode::LINE));
    EXPECT_EQ(reply, "second");
    client.disconnect();
    EXPECT_FALSE(client.is_connected());
  }
  t2.join();

  close(srv);
}

TEST(TcpClientTest, MoveSemantics) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::thread t(run_echo_server, srv);

  hps::TcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connect_to_server());
  EXPECT_TRUE(client.is_connected());

  hps::TcpClient moved(std::move(client));
  EXPECT_TRUE(moved.is_connected());
  EXPECT_FALSE(client.is_connected());

  EXPECT_TRUE(moved.send_message("data\n"));
  std::string reply;
  EXPECT_TRUE(moved.receive_message(reply, hps::ReadMode::LINE));
  EXPECT_EQ(reply, "data");

  moved.disconnect();
  t.join();
  close(srv);
}

TEST(TcpClientTest, SendPartialRetry) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::thread t(run_echo_server, srv);

  hps::TcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connect_to_server());

  std::string msg = "partial_test\n";
  EXPECT_TRUE(client.send_message(msg));

  std::string reply;
  EXPECT_TRUE(client.receive_message(reply, hps::ReadMode::LINE));
  EXPECT_EQ(reply, "partial_test");

  client.disconnect();
  t.join();
  close(srv);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
