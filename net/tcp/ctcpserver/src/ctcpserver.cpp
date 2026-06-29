#include "ctcpserver.h"

#include "logger.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>

namespace hps {

namespace {

constexpr int MAX_EPOLL_EVENTS = 64;

struct EpollFd {
  int fd;
};

struct EpollEvents {
  uint32_t flags;
};

int epoll_ctl_add(int epoll_fd, EpollFd target, EpollEvents ev_flags) {
  struct epoll_event ev {};

  ev.events = ev_flags.flags;
  ev.data.fd = target.fd;
  return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, target.fd, &ev);
}

int epoll_ctl_mod(int epoll_fd, EpollFd target, EpollEvents ev_flags) {
  struct epoll_event ev {};

  ev.events = ev_flags.flags;
  ev.data.fd = target.fd;
  return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, target.fd, &ev);
}

} // anonymous namespace

CTcpServer* CTcpServer::s_instance = nullptr;

void CTcpServer::signal_handler(int sig) {
  if (s_instance != nullptr) {
    Logger::_warn("收到信号 " + std::to_string(sig) + "，正在关闭服务器...");
    s_instance->stop();
  }
}

CTcpServer::CTcpServer(const Config& config) : config_(config) {}

CTcpServer::~CTcpServer() {
  stop();
  if (s_instance == this) {
    s_instance = nullptr;
  }
}

bool CTcpServer::init() {
  if (server_sockfd_ >= 0) {
    Logger::_warn("CTcpServer 已经初始化");
    return true;
  }

  if (s_instance != nullptr) {
    Logger::_error("CTcpServer 多实例不被支持，信号处理可能不准确");
    return false;
  }
  s_instance = this;

  server_sockfd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  if (server_sockfd_ < 0) {
    Logger::_error("创建 socket 失败: " + std::string(std::strerror(errno)));
    return false;
  }

  int optval = 1;
  if (setsockopt(server_sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
    Logger::_error("setsockopt SO_REUSEADDR 失败: " + std::string(std::strerror(errno)));
    close(server_sockfd_);
    server_sockfd_ = -1;
    return false;
  }

  struct sockaddr_in server_addr {};

  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(config_.port);

  if (bind(server_sockfd_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
    Logger::_error("bind 失败: " + std::string(std::strerror(errno)));
    close(server_sockfd_);
    server_sockfd_ = -1;
    return false;
  }

  {
    struct sockaddr_in bound_addr {};

    socklen_t bound_addr_len = sizeof(bound_addr);
    if (getsockname(server_sockfd_, reinterpret_cast<struct sockaddr*>(&bound_addr), &bound_addr_len) == 0) {
      actual_port_ = ntohs(bound_addr.sin_port);
    } else {
      actual_port_ = config_.port;
    }
  }

  if (listen(server_sockfd_, static_cast<int>(config_.backlog)) < 0) {
    Logger::_error("listen 失败: " + std::string(std::strerror(errno)));
    close(server_sockfd_);
    server_sockfd_ = -1;
    return false;
  }

  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    Logger::_error("epoll_create1 失败: " + std::string(std::strerror(errno)));
    close(server_sockfd_);
    server_sockfd_ = -1;
    return false;
  }

  wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ < 0) {
    Logger::_error("eventfd 失败: " + std::string(std::strerror(errno)));
    close(epoll_fd_);
    epoll_fd_ = -1;
    close(server_sockfd_);
    server_sockfd_ = -1;
    return false;
  }

  if (epoll_ctl_add(epoll_fd_, EpollFd{server_sockfd_}, EpollEvents{EPOLLIN | EPOLLET}) < 0) {
    Logger::_error("epoll_ctl ADD server fd 失败: " + std::string(std::strerror(errno)));
    close(wake_fd_);
    wake_fd_ = -1;
    close(epoll_fd_);
    epoll_fd_ = -1;
    close(server_sockfd_);
    server_sockfd_ = -1;
    return false;
  }

  if (epoll_ctl_add(epoll_fd_, EpollFd{wake_fd_}, EpollEvents{EPOLLIN}) < 0) {
    Logger::_error("epoll_ctl ADD wake fd 失败: " + std::string(std::strerror(errno)));
    close(wake_fd_);
    wake_fd_ = -1;
    close(epoll_fd_);
    epoll_fd_ = -1;
    close(server_sockfd_);
    server_sockfd_ = -1;
    return false;
  }

  struct sigaction sa {};

  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  thread_pool_ = std::make_unique<ThreadPool>(config_.thread_count);

  Logger::_info("CTcpServer 初始化成功，端口: " + std::to_string(config_.port) +
                ", 线程数: " + std::to_string(config_.thread_count));

  return true;
}

void CTcpServer::set_handler(Handler handler) {
  handler_ = std::move(handler);
}

void CTcpServer::start() {
  if (!running_.exchange(true)) {
    Logger::_info("CTcpServer 启动事件循环");
    event_loop();
  }
}

void CTcpServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }

  Logger::_info("CTcpServer 正在停止...");

  if (wake_fd_ >= 0) {
    uint64_t val = 1;
    [[maybe_unused]] auto unused = write(wake_fd_, &val, sizeof(val));
  }
}

void CTcpServer::event_loop() {
  std::array<struct epoll_event, MAX_EPOLL_EVENTS> events{};

  while (running_.load()) {
    int nfds = epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), config_.epoll_timeout_ms);
    if (nfds < 0) {
      if (errno == EINTR) {
        continue;
      }
      Logger::_error("epoll_wait 错误: " + std::string(std::strerror(errno)));
      break;
    }

    for (int i = 0; i < nfds; ++i) {
      handle_event(events.at(static_cast<size_t>(i)));
    }
  }

  cleanup_resources();
  Logger::_info("CTcpServer 已停止");
}

void CTcpServer::cleanup_resources() {
  for (auto& [fd, conn] : connections_) {
    conn->close();
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  }
  connections_.clear();

  if (wake_fd_ >= 0) {
    close(wake_fd_);
    wake_fd_ = -1;
  }
  if (epoll_fd_ >= 0) {
    close(epoll_fd_);
    epoll_fd_ = -1;
  }
  if (server_sockfd_ >= 0) {
    close(server_sockfd_);
    server_sockfd_ = -1;
  }

  signal(SIGINT, SIG_DFL);
  signal(SIGTERM, SIG_DFL);
}

void CTcpServer::handle_event(const struct epoll_event& evt) {
  const int fd = evt.data.fd;
  const uint32_t ev = evt.events;
  if (fd == server_sockfd_) {
    if ((ev & (EPOLLIN | EPOLLRDHUP)) != 0U) {
      if (!handle_accept()) {
        Logger::_error("accept 失败");
      }
    }
  } else if (fd == wake_fd_) {
    uint64_t val = 0;
    [[maybe_unused]] auto unused = read(wake_fd_, &val, sizeof(val));
    process_dirty_connections();
  } else {
    if ((ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
      if ((ev & (EPOLLHUP | EPOLLRDHUP)) != 0U) {
        handle_read(fd);
      }
      close_connection(fd);
    } else if ((ev & EPOLLIN) != 0U) {
      if (!handle_read(fd)) {
        close_connection(fd);
      }
    } else if ((ev & EPOLLOUT) != 0U) {
      if (!handle_write(fd)) {
        close_connection(fd);
      }
    }
  }
}

bool CTcpServer::handle_accept() {
  while (true) {
    struct sockaddr_in client_addr {};

    socklen_t client_addr_len = sizeof(client_addr);

    int client_fd =
      accept4(server_sockfd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_addr_len, SOCK_NONBLOCK);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      Logger::_error("accept4 失败: " + std::string(std::strerror(errno)));
      return false;
    }

    auto conn = std::make_shared<Connection>(client_fd, client_addr);

    if (epoll_ctl_add(epoll_fd_, EpollFd{client_fd}, EpollEvents{EPOLLIN | EPOLLET | EPOLLRDHUP}) < 0) {
      Logger::_error("epoll_ctl ADD client fd 失败: " + std::string(std::strerror(errno)));
      conn->close();
      continue;
    }

    connections_[client_fd] = conn;
    Logger::_info("新连接: " + conn->client_ip() + ":" + std::to_string(conn->client_port()) +
                  " (fd=" + std::to_string(client_fd) + ")");
  }

  return true;
}

bool CTcpServer::handle_read(int fd) {
  auto it = connections_.find(fd);
  if (it == connections_.end()) {
    return false;
  }

  auto& conn = it->second;
  ssize_t n = conn->read_from_fd();

  if (n < 0) {
    Logger::_error("读取数据失败 (fd=" + std::to_string(fd) + "): " + std::string(std::strerror(errno)));
    return false;
  }

  if (n == 0) {
    Logger::_info("客户端断开连接 (fd=" + std::to_string(fd) + ")");
    return false;
  }

  Logger::_info("收到 " + std::to_string(n) + " 字节 (fd=" + std::to_string(fd) + ")");

  if (handler_) {
    thread_pool_->enqueue([this, conn]() {
      handler_(conn);

      if (!conn->write_buffer().empty()) {
        conn->write_to_fd();
      }
      if (!conn->write_buffer().empty()) {
        {
          std::lock_guard<std::mutex> lock(dirty_mutex_);
          dirty_fds_.push_back(conn->fd());
        }
        notify_wake(wake_fd_);
      }
    });
  }

  return true;
}

bool CTcpServer::handle_write(int fd) {
  auto it = connections_.find(fd);
  if (it == connections_.end()) {
    return false;
  }

  auto& conn = it->second;

  conn->write_to_fd();

  if (conn->write_buffer().empty()) {
    epoll_ctl_mod(epoll_fd_, EpollFd{fd}, EpollEvents{EPOLLIN | EPOLLET | EPOLLRDHUP});
    Logger::_info("数据发送完成 (fd=" + std::to_string(fd) + ")");
  } else {
    Logger::_info("部分发送，等待 EPOLLOUT (fd=" + std::to_string(fd) + ")");
  }

  return true;
}

void CTcpServer::close_connection(int fd) {
  auto it = connections_.find(fd);
  if (it == connections_.end()) {
    return;
  }

  Logger::_info("关闭连接 (fd=" + std::to_string(fd) + ")");

  it->second->close();
  epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  connections_.erase(it);
}

void CTcpServer::notify_wake(int wake_fd) {
  if (wake_fd < 0) {
    return;
  }
  uint64_t val = 1;
  [[maybe_unused]] auto unused = write(wake_fd, &val, sizeof(val));
}

void CTcpServer::process_dirty_connections() {
  std::vector<int> fds;
  {
    std::lock_guard<std::mutex> lock(dirty_mutex_);
    fds.swap(dirty_fds_);
  }

  for (int fd : fds) {
    if (!connections_.contains(fd)) {
      continue;
    }
    connections_[fd]->write_to_fd();
    if (!connections_[fd]->write_buffer().empty()) {
      epoll_ctl_mod(epoll_fd_, EpollFd{fd}, EpollEvents{EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP});
    }
  }
}

} // namespace hps
