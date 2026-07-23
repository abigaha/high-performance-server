#include "tcp_server.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <future>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class TcpServerTest : public ::testing::Test {
protected:
  hps::TcpServer::Config config_;
  std::unique_ptr<hps::TcpServer> server_;
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
    server_ = std::make_unique<hps::TcpServer>(config_);
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

TEST_F(TcpServerTest, InitSuccess) {
  hps::TcpServer server(config_);
  EXPECT_TRUE(server.init());
  EXPECT_GT(server.actual_port(), 0);
}

TEST_F(TcpServerTest, InitDuplicatePort) {
  hps::TcpServer server1(config_);
  ASSERT_TRUE(server1.init());

  hps::TcpServer::Config same_port_config;
  same_port_config.port = server1.actual_port();

  hps::TcpServer server2(same_port_config);
  EXPECT_FALSE(server2.init()); // 端口已被占用
}

TEST_F(TcpServerTest, StartStop) {
  start_server();

  EXPECT_TRUE(server_->is_running());

  server_->stop();
  server_thread_.join();
  server_thread_ = std::thread();

  EXPECT_FALSE(server_->is_running());
}

TEST_F(TcpServerTest, SingleConnectionEcho) {
  std::atomic<bool> handler_called{false};
  std::string received_data;

  config_.port = 0;
  server_ = std::make_unique<hps::TcpServer>(config_);
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

TEST_F(TcpServerTest, MultipleConcurrentConnections) {
  std::atomic<int> handler_count{0};
  constexpr int NUM_CLIENTS = 10;

  config_.port = 0;
  server_ = std::make_unique<hps::TcpServer>(config_);
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

TEST_F(TcpServerTest, StopWaitsForActiveHandlerBeforeStartReturns) {
  constexpr auto kWaitTimeout = std::chrono::milliseconds(3000);
  constexpr auto kBlockedObservationWindow = std::chrono::milliseconds(200);
  std::latch release_handler(1);
  std::atomic<bool> handler_entered{false};
  std::atomic<bool> handler_finished{false};
  std::atomic<bool> start_returned{false};

  const auto wait_until = [](const auto& condition, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
  };

  config_.thread_count = 1;
  server_ = std::make_unique<hps::TcpServer>(config_);
  server_->set_handler([&](std::shared_ptr<hps::Connection>) {
    handler_entered.store(true, std::memory_order_release);
    release_handler.wait();
    handler_finished.store(true, std::memory_order_release);
  });
  ASSERT_TRUE(server_->init());

  server_thread_ = std::thread([this, &start_returned]() {
    server_->start();
    start_returned.store(true, std::memory_order_release);
  });

  const bool server_started = wait_until([this] { return server_->is_running(); }, kWaitTimeout);
  EXPECT_TRUE(server_started);

  int client_fd = -1;
  bool handler_was_entered = false;
  if (server_started) {
    client_fd = connect_client();
    EXPECT_GE(client_fd, 0);
    if (client_fd >= 0) {
      const std::string request = "block";
      const ssize_t sent = send(client_fd, request.data(), request.size(), 0);
      const bool request_sent = sent == static_cast<ssize_t>(request.size());
      EXPECT_TRUE(request_sent);

      if (request_sent) {
        handler_was_entered =
          wait_until([&handler_entered] { return handler_entered.load(std::memory_order_acquire); }, kWaitTimeout);
        EXPECT_TRUE(handler_was_entered);

        if (handler_was_entered) {
          server_->stop();
          const bool returned_while_handler_blocked =
            wait_until([&start_returned] { return start_returned.load(std::memory_order_acquire); },
                       kBlockedObservationWindow);
          EXPECT_FALSE(returned_while_handler_blocked);
        }
      }
    }
  }

  release_handler.count_down();
  if (client_fd >= 0) {
    close(client_fd);
  }
  server_->stop();

  if (handler_was_entered) {
    EXPECT_TRUE(
      wait_until([&handler_finished] { return handler_finished.load(std::memory_order_acquire); }, kWaitTimeout));
  }

  const bool start_returned_after_release =
    wait_until([&start_returned] { return start_returned.load(std::memory_order_acquire); }, kWaitTimeout);
  EXPECT_TRUE(start_returned_after_release);
  if (start_returned_after_release && server_thread_.joinable()) {
    server_thread_.join();
    server_thread_ = std::thread();
  }
}

TEST_F(TcpServerTest, Accepts256SynchronizedConcurrentConnections) {
  constexpr std::size_t kConcurrentClientCount = 256;
  constexpr int kConnectTimeoutMs = 5000;
  std::atomic<std::size_t> successful_connections{0};
  std::vector<int> client_fds(kConcurrentClientCount, -1);
  std::vector<std::thread> client_threads;
  std::latch start_latch(static_cast<std::ptrdiff_t>(kConcurrentClientCount + 1));

  config_.backlog = kConcurrentClientCount;
  start_server();
  const uint16_t server_port = server_->actual_port();
  ASSERT_GT(server_port, 0);

  client_threads.reserve(kConcurrentClientCount);
  for (std::size_t client_index = 0; client_index < kConcurrentClientCount; ++client_index) {
    client_threads.emplace_back([&, client_index] {
      start_latch.arrive_and_wait();

      const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
      if (fd < 0) {
        return;
      }

      struct sockaddr_in address {};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = htons(server_port);

      const int connect_result = connect(fd,
                                         // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                                         reinterpret_cast<const struct sockaddr*>(&address),
                                         sizeof(address));
      bool connected = connect_result == 0;
      if (!connected && errno == EINPROGRESS) {
        struct pollfd poll_fd {};
        poll_fd.fd = fd;
        poll_fd.events = POLLOUT;

        if (poll(&poll_fd, 1, kConnectTimeoutMs) > 0) {
          int socket_error = 0;
          socklen_t socket_error_size = sizeof(socket_error);
          connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) == 0 && socket_error == 0;
        }
      }

      if (!connected) {
        close(fd);
        return;
      }

      client_fds[client_index] = fd;
      successful_connections.fetch_add(1, std::memory_order_relaxed);
    });
  }

  start_latch.arrive_and_wait();
  for (auto& client_thread : client_threads) {
    client_thread.join();
  }

  EXPECT_EQ(successful_connections.load(std::memory_order_relaxed), kConcurrentClientCount);

  for (const int fd : client_fds) {
    if (fd >= 0) {
      close(fd);
    }
  }
}

TEST_F(TcpServerTest, NoHandlerNoCrash) {
  config_.port = 0;
  server_ = std::make_unique<hps::TcpServer>(config_);
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

TEST_F(TcpServerTest, ClientDisconnect) {
  std::atomic<int> connect_count{0};
  std::atomic<int> close_detected{0};

  config_.port = 0;
  server_ = std::make_unique<hps::TcpServer>(config_);
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

TEST_F(TcpServerTest, NonPositiveEpollTimeoutDoesNotBusyWait) {
  constexpr auto kIdleObservation = std::chrono::milliseconds(150);
  constexpr auto kMaximumIdleCpuTime = std::chrono::milliseconds(50);
  config_.epoll_timeout_ms = -1;
  start_server();

  struct timespec cpu_before {};

  ASSERT_EQ(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_before), 0);
  std::this_thread::sleep_for(kIdleObservation);

  struct timespec cpu_after {};

  ASSERT_EQ(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_after), 0);

  const auto to_duration = [](const struct timespec& timestamp) {
    return std::chrono::seconds(timestamp.tv_sec) + std::chrono::nanoseconds(timestamp.tv_nsec);
  };
  EXPECT_LT(to_duration(cpu_after) - to_duration(cpu_before), kMaximumIdleCpuTime);
}

TEST_F(TcpServerTest, ZeroEpollTimeoutStillHandlesRealConnection) {
  constexpr auto kHandlerTimeout = std::chrono::seconds(2);
  config_.epoll_timeout_ms = 0;
  auto handled_request = std::make_shared<std::promise<std::string>>();
  auto handled_request_future = handled_request->get_future();
  auto handler_completed = std::make_shared<std::atomic<bool>>(false);

  server_ = std::make_unique<hps::TcpServer>(config_);
  server_->set_handler([handled_request, handler_completed](std::shared_ptr<hps::Connection> conn) {
    if (!handler_completed->exchange(true, std::memory_order_acq_rel)) {
      handled_request->set_value(conn->read_buffer());
    }
  });
  ASSERT_TRUE(server_->init());
  server_thread_ = std::thread([this]() { server_->start(); });

  const auto start_deadline = std::chrono::steady_clock::now() + kHandlerTimeout;
  while (!server_->is_running() && std::chrono::steady_clock::now() < start_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(server_->is_running());

  const int client_fd = connect_client();
  ASSERT_GE(client_fd, 0);
  const std::string request = "zero-timeout";
  ASSERT_EQ(send(client_fd, request.data(), request.size(), 0), static_cast<ssize_t>(request.size()));
  ASSERT_EQ(handled_request_future.wait_for(kHandlerTimeout), std::future_status::ready);
  EXPECT_EQ(handled_request_future.get(), request);

  close(client_fd);
}

TEST_F(TcpServerTest, StopWakeFdInterruptsLongEpollWait) {
  constexpr auto kStopTimeout = std::chrono::seconds(1);
  config_.epoll_timeout_ms = 5000;
  start_server();

  auto stop_future = std::async(std::launch::async, [this] { server_->stop(); });
  ASSERT_EQ(stop_future.wait_for(kStopTimeout), std::future_status::ready);
  stop_future.get();

  server_thread_.join();
  server_thread_ = std::thread();
  EXPECT_FALSE(server_->is_running());
}

TEST_F(TcpServerTest, SignalStopServer) {
  config_.port = 0;
  server_ = std::make_unique<hps::TcpServer>(config_);
  ASSERT_TRUE(server_->init());
  server_thread_ = std::thread([this]() { server_->start(); });
  while (!server_->is_running()) {
    std::this_thread::yield();
  }

  EXPECT_TRUE(server_->is_running());

  // 发送 SIGINT 模拟 Ctrl-C
  raise(SIGINT);

  // 等待服务器停止（超时 3 秒）
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (server_->is_running() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_FALSE(server_->is_running());

  server_thread_.join();
  server_thread_ = std::thread();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
