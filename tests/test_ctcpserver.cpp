#include "ctcpserver.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

class CTcpServerTest : public ::testing::Test {
protected:
  hps::CTcpServer::Config config_;
  std::unique_ptr<hps::CTcpServer> server_;
  std::thread server_thread_;
  std::atomic<bool> server_ready_{false};

  void SetUp() override {
    config_.port = 0; // 自动分配端口
    config_.thread_count = 2;
    config_.epoll_timeout_ms = 50;
  }

  void TearDown() override {
    if (server_) {
      server_->stop();
    }
    if (server_thread_.joinable()) {
      server_thread_.join();
    }
  }

  void start_server() {
    server_ = std::make_unique<hps::CTcpServer>(config_);
    ASSERT_TRUE(server_->init());
    server_ready_ = true;
    server_thread_ = std::thread([this]() { server_->start(); });
    // 等待服务器开始运行
    while (!server_->is_running()) {
      std::this_thread::yield();
    }
  }

  int connect_client() {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    EXPECT_GE(fd, 0);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(server_->actual_port());

    int ret = connect(fd,
                      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                      reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr));
    if (ret < 0) {
      close(fd);
      return -1;
    }

    return fd;
  }
};

TEST_F(CTcpServerTest, InitSuccess) {
  hps::CTcpServer server(config_);
  EXPECT_TRUE(server.init());
  EXPECT_GT(server.actual_port(), 0);
}

TEST_F(CTcpServerTest, InitDuplicatePort) {
  hps::CTcpServer server1(config_);
  ASSERT_TRUE(server1.init());

  hps::CTcpServer::Config same_port_config;
  same_port_config.port = server1.actual_port();

  hps::CTcpServer server2(same_port_config);
  EXPECT_FALSE(server2.init()); // 端口已被占用
}

TEST_F(CTcpServerTest, StartStop) {
  start_server();

  EXPECT_TRUE(server_->is_running());

  server_->stop();
  server_thread_.join();
  server_thread_ = std::thread();

  EXPECT_FALSE(server_->is_running());
}

TEST_F(CTcpServerTest, SingleConnectionEcho) {
  std::atomic<bool> handler_called{false};
  std::string received_data;

  config_.port = 0;
  server_ = std::make_unique<hps::CTcpServer>(config_);
  server_->set_handler([&](std::shared_ptr<hps::Connection> conn) {
    handler_called = true;
    received_data = conn->read_buffer();
    conn->write_buffer() = "echo: " + conn->read_buffer();
  });
  ASSERT_TRUE(server_->init());
  server_ready_ = true;
  server_thread_ = std::thread([this]() { server_->start(); });
  while (!server_->is_running()) {
    std::this_thread::yield();
  }

  int client_fd = connect_client();
  ASSERT_GE(client_fd, 0);

  std::string msg = "Hello, Server!";
  (void)send(client_fd, msg.data(), msg.size(), 0);

  // 等待 handler 被调用
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_TRUE(handler_called);
  EXPECT_EQ(received_data, msg);

  // 读取 echo 响应
  std::string expected_response = "echo: Hello, Server!";
  char buf[128];
  std::memset(buf, 0, sizeof(buf));
  // 多次读取直到完整响应
  std::string response;
  auto start_time = std::chrono::steady_clock::now();
  while (response.size() < expected_response.size() &&
         std::chrono::steady_clock::now() - start_time < std::chrono::seconds(2)) {
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      response.append(buf, static_cast<size_t>(n));
    }
  }
  EXPECT_EQ(response, expected_response);

  close(client_fd);
  server_->stop();
  server_thread_.join();
  server_thread_ = std::thread();
}

TEST_F(CTcpServerTest, MultipleConcurrentConnections) {
  std::atomic<int> handler_count{0};
  constexpr int NUM_CLIENTS = 10;

  config_.port = 0;
  server_ = std::make_unique<hps::CTcpServer>(config_);
  server_->set_handler([&](std::shared_ptr<hps::Connection> conn) {
    handler_count++;
    conn->write_buffer() = "OK";
  });
  ASSERT_TRUE(server_->init());
  server_thread_ = std::thread([this]() { server_->start(); });
  while (!server_->is_running()) {
    std::this_thread::yield();
  }

  std::vector<int> client_fds;
  for (int i = 0; i < NUM_CLIENTS; ++i) {
    int fd = connect_client();
    if (fd >= 0) {
      client_fds.push_back(fd);
      std::string msg = "msg" + std::to_string(i);
      (void)send(fd, msg.data(), msg.size(), 0);
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  EXPECT_EQ(handler_count.load(), NUM_CLIENTS);

  for (int fd : client_fds) {
    close(fd);
  }

  server_->stop();
  server_thread_.join();
  server_thread_ = std::thread();
}

TEST_F(CTcpServerTest, NoHandlerNoCrash) {
  config_.port = 0;
  server_ = std::make_unique<hps::CTcpServer>(config_);
  ASSERT_TRUE(server_->init());
  server_thread_ = std::thread([this]() { server_->start(); });
  while (!server_->is_running()) {
    std::this_thread::yield();
  }

  int client_fd = connect_client();
  ASSERT_GE(client_fd, 0);
  (void)send(client_fd, "data", 4, 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  // 不设 handler，不应崩溃

  close(client_fd);
  server_->stop();
  server_thread_.join();
  server_thread_ = std::thread();
}

TEST_F(CTcpServerTest, ClientDisconnect) {
  std::atomic<int> connect_count{0};
  std::atomic<int> close_detected{0};

  config_.port = 0;
  server_ = std::make_unique<hps::CTcpServer>(config_);
  server_->set_handler([&](std::shared_ptr<hps::Connection> conn) {
    connect_count++;
    conn->write_buffer() = "OK";
  });
  ASSERT_TRUE(server_->init());
  server_thread_ = std::thread([this]() { server_->start(); });
  while (!server_->is_running()) {
    std::this_thread::yield();
  }

  {
    int client_fd = connect_client();
    ASSERT_GE(client_fd, 0);
    (void)send(client_fd, "hello", 5, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    close(client_fd); // 主动断开
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(connect_count.load(), 1);

  server_->stop();
  server_thread_.join();
  server_thread_ = std::thread();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
