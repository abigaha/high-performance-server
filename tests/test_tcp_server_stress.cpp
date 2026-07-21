#include "connection.h"
#include "tcp_server.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace hps;

namespace {

class TcpServerStressTest : public ::testing::Test {
protected:
  TcpServer::Config config_;
  std::unique_ptr<TcpServer> server_;
  std::thread server_thread_;

  void SetUp() override {
    config_.port = 0;
    config_.thread_count = 4;
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
    server_ = std::make_unique<TcpServer>(config_);
    ASSERT_TRUE(server_->init());
    server_thread_ = std::thread([this]() { server_->start(); });
    while (!server_->is_running()) {
      std::this_thread::yield();
    }
  }

  int connect_client() {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
      return -1;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(server_->actual_port());
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      close(fd);
      return -1;
    }
    return fd;
  }
};

} // namespace

TEST_F(TcpServerStressTest, ManyConcurrentClients) {
  constexpr int kNumClients = 10;
  std::atomic<int> handler_count{0};

  config_.port = 0;
  server_ = std::make_unique<TcpServer>(config_);
  server_->set_handler([&](std::shared_ptr<Connection> conn) {
    handler_count.fetch_add(1, std::memory_order_relaxed);
    conn->write_buffer() = "OK";
  });
  ASSERT_TRUE(server_->init());
  server_thread_ = std::thread([this]() { server_->start(); });
  while (!server_->is_running()) {
    std::this_thread::yield();
  }

  std::vector<int> clients;
  for (int i = 0; i < kNumClients; ++i) {
    int fd = connect_client();
    if (fd >= 0) {
      clients.push_back(fd);
      std::string msg = "msg" + std::to_string(i);
      send(fd, msg.data(), msg.size(), 0);
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_EQ(handler_count.load(), kNumClients);

  for (int fd : clients) {
    close(fd);
  }
  server_->stop();
  server_thread_.join();
  server_thread_ = std::thread();
}

TEST_F(TcpServerStressTest, ClientDisconnectUnexpectedly) {
  std::atomic<bool> handler_called{false};

  config_.port = 0;
  server_ = std::make_unique<TcpServer>(config_);
  server_->set_handler([&](std::shared_ptr<Connection> conn) {
    handler_called = true;
    conn->write_buffer() = "reply";
  });
  ASSERT_TRUE(server_->init());
  server_thread_ = std::thread([this]() { server_->start(); });
  while (!server_->is_running()) {
    std::this_thread::yield();
  }

  {
    int fd = connect_client();
    ASSERT_GE(fd, 0);
    send(fd, "hello", 5, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    close(fd);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(handler_called);
  EXPECT_TRUE(server_->is_running());

  server_->stop();
  server_thread_.join();
  server_thread_ = std::thread();
}

TEST_F(TcpServerStressTest, LargeDataFromMultipleClients) {
  constexpr int kNumClients = 5;
  constexpr int kDataSize = 65536;
  std::array<std::atomic<bool>, kNumClients> processed_clients;
  std::atomic<int> processed_client_count{0};
  std::atomic<bool> invalid_payload{false};

  for (auto& processed : processed_clients) {
    processed.store(false, std::memory_order_relaxed);
  }

  config_.port = 0;
  server_ = std::make_unique<TcpServer>(config_);
  server_->set_handler([&](std::shared_ptr<Connection> conn) {
    std::string payload;
    {
      std::lock_guard<std::mutex> read_lock(conn->read_mutex());
      payload = conn->read_buffer();
    }

    if (payload.size() < kDataSize) {
      return;
    }

    const char marker = payload.front();
    if (payload.size() != kDataSize || marker < 'A' || marker >= 'A' + kNumClients ||
        !std::all_of(payload.begin(), payload.end(),
                     [marker](char byte) { return byte == marker; })) {
      invalid_payload.store(true, std::memory_order_relaxed);
      return;
    }

    const auto client_index = static_cast<size_t>(marker - 'A');
    if (!processed_clients.at(client_index).exchange(true, std::memory_order_relaxed)) {
      processed_client_count.fetch_add(1, std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> write_lock(conn->write_mutex());
    conn->write_buffer() = "OK";
  });
  ASSERT_TRUE(server_->init());
  server_thread_ = std::thread([this]() { server_->start(); });
  while (!server_->is_running()) {
    std::this_thread::yield();
  }

  std::vector<int> clients;
  for (int i = 0; i < kNumClients; ++i) {
    int fd = connect_client();
    if (fd >= 0) {
      clients.push_back(fd);
      std::string data(kDataSize, static_cast<char>('A' + i));
      size_t bytes_sent = 0;
      while (bytes_sent < data.size()) {
        const ssize_t sent = send(fd, data.data() + bytes_sent, data.size() - bytes_sent, 0);
        ASSERT_GT(sent, 0);
        bytes_sent += static_cast<size_t>(sent);
      }
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (processed_client_count.load(std::memory_order_relaxed) < kNumClients &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_FALSE(invalid_payload.load(std::memory_order_relaxed));
  EXPECT_EQ(processed_client_count.load(std::memory_order_relaxed), kNumClients);
  for (int client_index = 0; client_index < kNumClients; ++client_index) {
    EXPECT_TRUE(processed_clients.at(static_cast<size_t>(client_index))
                    .load(std::memory_order_relaxed));
  }

  for (int fd : clients) {
    close(fd);
  }
  server_->stop();
  server_thread_.join();
  server_thread_ = std::thread();
}

TEST_F(TcpServerStressTest, ServerMaxConnections) {
  std::atomic<int> conn_count{0};

  config_.port = 0;
  config_.backlog = 3;
  server_ = std::make_unique<TcpServer>(config_);
  server_->set_handler([&](std::shared_ptr<Connection> conn) {
    conn_count.fetch_add(1, std::memory_order_relaxed);
    conn->write_buffer() = "OK";
  });
  ASSERT_TRUE(server_->init());
  server_thread_ = std::thread([this]() { server_->start(); });
  while (!server_->is_running()) {
    std::this_thread::yield();
  }

  std::vector<int> clients;
  for (int i = 0; i < 5; ++i) {
    int fd = connect_client();
    if (fd >= 0) {
      clients.push_back(fd);
      send(fd, "data", 4, 0);
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  for (int fd : clients) {
    close(fd);
  }
  server_->stop();
  server_thread_.join();
  server_thread_ = std::thread();
}

TEST_F(TcpServerStressTest, ServerStopWhileClientsConnected) {
  std::atomic<bool> handler_called{false};

  config_.port = 0;
  server_ = std::make_unique<TcpServer>(config_);
  server_->set_handler([&](std::shared_ptr<Connection> conn) {
    handler_called = true;
    conn->write_buffer() = "reply";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  });
  ASSERT_TRUE(server_->init());
  server_thread_ = std::thread([this]() { server_->start(); });
  while (!server_->is_running()) {
    std::this_thread::yield();
  }

  int fd = connect_client();
  ASSERT_GE(fd, 0);
  send(fd, "hello", 5, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  server_->stop();
  server_thread_.join();
  server_thread_ = std::thread();
  close(fd);

  EXPECT_TRUE(handler_called);
}

TEST_F(TcpServerStressTest, ServerRestart) {
  config_.port = 0;
  {
    TcpServer srv(config_);
    ASSERT_TRUE(srv.init());
    EXPECT_GT(srv.actual_port(), 0);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  TcpServer srv2(config_);
  ASSERT_TRUE(srv2.init());
  EXPECT_GT(srv2.actual_port(), 0);
  srv2.stop();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
