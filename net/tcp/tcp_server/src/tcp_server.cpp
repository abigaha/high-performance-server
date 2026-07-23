#include "tcp_server.h"

#include "logger.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
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

constexpr int kMaxEpollEvents = 64;
constexpr int kMinimumEpollTimeoutMs = 1;

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

TcpServer* TcpServer::s_instance_ = nullptr;

void TcpServer::signal_handler(int sig) {
  if (s_instance_ != nullptr) {
    Logger::_warn("收到信号 " + std::to_string(sig) + "，正在关闭服务器...");
    s_instance_->stop();
  }
}

TcpServer::TcpServer(const Config& config) : config_(config) {
  if (config_.epoll_timeout_ms <= 0) {
    Logger::_warn("epoll_timeout_ms 必须大于 0，已调整为 " + std::to_string(kMinimumEpollTimeoutMs) + " 毫秒");
    config_.epoll_timeout_ms = kMinimumEpollTimeoutMs;
  }
}

TcpServer::~TcpServer() {
  this->TcpServer::stop(); // 限定调用避免析构时虚分发绕过
  if (s_instance_ == this) {
    s_instance_ = nullptr;
  }
}

bool TcpServer::init() {
  if (server_sockfd_ >= 0) {
    Logger::_warn("TcpServer 已经初始化");
    return true;
  }

  if (s_instance_ != nullptr) {
    Logger::_error("TcpServer 多实例不被支持，信号处理可能不准确");
    return false;
  }
  s_instance_ = this;

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

  thread_pool_ = std::make_unique<LockFreeThreadPool>(config_.thread_count);

  if (config_.ssl_config.enabled) {
    try {
      ssl_context_ = std::make_unique<SslContext>(config_.ssl_config);
    } catch (const std::exception& e) {
      Logger::_error("SSL 上下文初始化失败: " + std::string(e.what()));
      cleanup_resources();
      return false;
    }
    Logger::_info("SSL 已启用");
  }

  Logger::_info("TcpServer 初始化成功，端口: " + std::to_string(config_.port) +
                ", 线程数: " + std::to_string(config_.thread_count));

  return true;
}

void TcpServer::set_handler(Handler handler) {
  handler_ = std::move(handler);
}

void TcpServer::start() {
  if (!running_.exchange(true)) {
    Logger::_info("TcpServer 启动事件循环");
    event_loop();
  }
}

void TcpServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }

  Logger::_info("TcpServer 正在停止...");

  if (wake_fd_ >= 0) {
    uint64_t val = 1;
    [[maybe_unused]] auto unused = write(wake_fd_, &val, sizeof(val));
  }
}

void TcpServer::event_loop() {
  std::array<struct epoll_event, kMaxEpollEvents> events{};

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
  Logger::_info("TcpServer 已停止");
}

void TcpServer::cleanup_resources() {
  if (thread_pool_ != nullptr) {
    thread_pool_->stop();
  }

  for (auto& [fd, conn] : connections_) {
    conn->close();
    if (close_handler_) {
      close_handler_(conn.get());
    }
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

void TcpServer::handle_event(const struct epoll_event& evt) {
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
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
      return;
    }
    auto& conn = it->second;

    if (try_ssl_detection(conn, evt) || try_ssl_handshake(conn, evt)) {
      return;
    }
    handle_client_event(fd, evt);
  }
}

bool TcpServer::handle_accept() {
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

bool TcpServer::handle_read(int fd) {
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

      // N1-H：write_to_fd 与 write_buffer 检查须持 write_mutex
      bool need_dirty = false;
      {
        std::lock_guard<std::mutex> wlock(conn->write_mutex());
        conn->write_to_fd_locked();
        if (!conn->write_buffer().empty()) {
          need_dirty = true;
        }
      }
      if (need_dirty) {
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

bool TcpServer::handle_write(int fd) {
  auto it = connections_.find(fd);
  if (it == connections_.end()) {
    return false;
  }

  bool done = false;
  {
    auto& conn = it->second;
    std::lock_guard<std::mutex> wlock(conn->write_mutex());
    conn->write_to_fd_locked();
    done = conn->write_buffer().empty();
  }

  if (done) {
    epoll_ctl_mod(epoll_fd_, EpollFd{fd}, EpollEvents{EPOLLIN | EPOLLET | EPOLLRDHUP});
    Logger::_info("数据发送完成 (fd=" + std::to_string(fd) + ")");
  } else {
    Logger::_info("部分发送，等待 EPOLLOUT (fd=" + std::to_string(fd) + ")");
  }

  return true;
}

bool TcpServer::try_ssl_detection(std::shared_ptr<Connection>& conn, const struct epoll_event& ev) {
  if (!config_.ssl_config.enabled) {
    return false;
  }
  if (conn->ssl_state() != Connection::SslState::NONE) {
    return false;
  }
  if ((ev.events & EPOLLIN) == 0U) {
    return false;
  }

  std::array<char, 1> peek_buf{};
  ssize_t n = ::recv(conn->fd(), peek_buf.data(), 1, MSG_PEEK);
  if (n != 1 || static_cast<unsigned char>(peek_buf[0]) != 0x16) {
    return false;
  }

  auto* ssl = ssl_context_->create_ssl();
  conn->set_ssl(ssl);
  SSL_set_fd(static_cast<SSL*>(ssl), conn->fd());
  conn->set_ssl_state(Connection::SslState::HANDSHAKE);
  handle_ssl_handshake(conn, ev);
  return true;
}

bool TcpServer::try_ssl_handshake(std::shared_ptr<Connection>& conn, const struct epoll_event& ev) {
  if (conn->ssl_state() != Connection::SslState::HANDSHAKE) {
    return false;
  }
  handle_ssl_handshake(conn, ev);
  return true;
}

void TcpServer::handle_client_event(int fd, const struct epoll_event& evt) {
  const uint32_t ev = evt.events;
  if ((ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
    if ((ev & (EPOLLHUP | EPOLLRDHUP)) != 0U) {
      if (!handle_read(fd)) {
        close_connection(fd);
        return;
      }
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

void TcpServer::handle_ssl_handshake(std::shared_ptr<Connection>& conn, const struct epoll_event& ev) {
  int ret = SSL_accept(static_cast<SSL*>(conn->ssl()));
  if (ret == 1) {
    conn->set_ssl_state(Connection::SslState::ESTABLISHED);
    epoll_ctl_mod(epoll_fd_, EpollFd{conn->fd()}, EpollEvents{EPOLLIN | EPOLLET | EPOLLRDHUP});
    Logger::_info("TLS 握手完成 (fd=" + std::to_string(conn->fd()) + ")");
    if ((ev.events & EPOLLIN) != 0U) {
      handle_read(conn->fd());
    }
    return;
  }

  int err = SSL_get_error(static_cast<SSL*>(conn->ssl()), ret);
  if (err == SSL_ERROR_WANT_READ) {
    return;
  }
  if (err == SSL_ERROR_WANT_WRITE) {
    epoll_ctl_mod(epoll_fd_, EpollFd{conn->fd()}, EpollEvents{EPOLLOUT | EPOLLET | EPOLLRDHUP});
    return;
  }

  auto ssl_err = ERR_get_error();
  std::array<char, 256> err_buf{};
  ERR_error_string_n(ssl_err, err_buf.data(), err_buf.size());
  Logger::_error("SSL_accept 失败 (fd=" + std::to_string(conn->fd()) + "): " + std::string(err_buf.data()));
  close_connection(conn->fd());
}

void TcpServer::close_connection(int fd) {
  auto it = connections_.find(fd);
  if (it == connections_.end()) {
    return;
  }

  Logger::_info("关闭连接 (fd=" + std::to_string(fd) + ")");

  it->second->close();
  if (close_handler_) {
    close_handler_(it->second.get());
  }
  epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  connections_.erase(it);
}

void TcpServer::notify_wake(int wake_fd) {
  if (wake_fd < 0) {
    return;
  }
  uint64_t val = 1;
  [[maybe_unused]] auto unused = write(wake_fd, &val, sizeof(val));
}

void TcpServer::process_dirty_connections() {
  std::vector<int> fds;
  {
    std::lock_guard<std::mutex> lock(dirty_mutex_);
    fds.swap(dirty_fds_);
  }

  for (int fd : fds) {
    if (!connections_.contains(fd)) {
      continue;
    }
    bool still_dirty = false;
    {
      auto& conn = connections_[fd];
      std::lock_guard<std::mutex> wlock(conn->write_mutex());
      conn->write_to_fd_locked();
      if (!conn->write_buffer().empty()) {
        still_dirty = true;
      }
    }
    if (still_dirty) {
      epoll_ctl_mod(epoll_fd_, EpollFd{fd}, EpollEvents{EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP});
    }
  }
}

} // namespace hps
