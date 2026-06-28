#include "ctcpclient.h"

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
  return std::string(tmpdir) + "/opencode_test_ctcpclient_" + std::to_string(getpid());
}

} // anonymous namespace

TEST(CTcpClientTest, ConnectTimeout) {
  hps::CTcpClient client(100, "127.0.0.1", 1);
  EXPECT_FALSE(client.connectToServer());
  EXPECT_FALSE(client.is_connected());
}

TEST(CTcpClientTest, SendReceiveLine) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::thread t(run_echo_server, srv);

  hps::CTcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connectToServer());
  EXPECT_TRUE(client.is_connected());

  EXPECT_TRUE(client.sendMessage("hello\n"));
  std::string reply;
  EXPECT_TRUE(client.receiveMessage(reply, hps::ReadMode::Line));
  EXPECT_EQ(reply, "hello");

  client.disconnect();
  t.join();
  close(srv);
}

TEST(CTcpClientTest, ReceiveTimeout) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::thread t(run_echo_server, srv);

  hps::CTcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connectToServer());

  std::string reply;
  EXPECT_FALSE(client.receiveMessage(reply, hps::ReadMode::Raw, 100));

  client.disconnect();
  t.join();
  close(srv);
}

TEST(CTcpClientTest, SendFile) {
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

  hps::CTcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connectToServer());
  EXPECT_TRUE(client.sendFile(path));

  std::string reply;
  EXPECT_TRUE(client.receiveMessage(reply, hps::ReadMode::Raw));
  EXPECT_EQ(reply, "file_content_123");

  client.disconnect();
  t.join();
  close(srv);
  std::remove(path.c_str());
}

TEST(CTcpClientTest, SendLargeData) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::string large_data(10000, 'x');

  std::thread t(run_echo_server, srv);

  hps::CTcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connectToServer());
  EXPECT_TRUE(client.sendFile(large_data, large_data.size()));

  std::string reply;
  EXPECT_TRUE(client.receiveMessage(reply, hps::ReadMode::Raw));
  EXPECT_EQ(reply.size(), large_data.size());

  client.disconnect();
  t.join();
  close(srv);
}

TEST(CTcpClientTest, DisconnectReconnect) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::thread t1(run_echo_server, srv);

  hps::CTcpClient client("127.0.0.1", static_cast<uint16_t>(port));

  ASSERT_TRUE(client.connectToServer());
  EXPECT_TRUE(client.is_connected());
  client.disconnect();
  EXPECT_FALSE(client.is_connected());

  ASSERT_TRUE(client.connectToServer());
  EXPECT_TRUE(client.is_connected());

  client.disconnect();
  t1.join();
  close(srv);
}

TEST(CTcpClientTest, MoveSemantics) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::thread t(run_echo_server, srv);

  hps::CTcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connectToServer());
  EXPECT_TRUE(client.is_connected());

  hps::CTcpClient moved(std::move(client));
  EXPECT_TRUE(moved.is_connected());
  EXPECT_FALSE(client.is_connected());

  EXPECT_TRUE(moved.sendMessage("data\n"));
  std::string reply;
  EXPECT_TRUE(moved.receiveMessage(reply, hps::ReadMode::Line));
  EXPECT_EQ(reply, "data");

  moved.disconnect();
  t.join();
  close(srv);
}

TEST(CTcpClientTest, SendPartialRetry) {
  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::thread t(run_echo_server, srv);

  hps::CTcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connectToServer());

  std::string msg = "partial_test\n";
  EXPECT_TRUE(client.sendMessage(msg));

  std::string reply;
  EXPECT_TRUE(client.receiveMessage(reply, hps::ReadMode::Line));
  EXPECT_EQ(reply, "partial_test");

  client.disconnect();
  t.join();
  close(srv);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
