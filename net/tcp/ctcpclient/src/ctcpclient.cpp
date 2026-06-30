#include "ctcpclient.h"

#include "logger.h"

#include <poll.h>

#include <array>
#include <cerrno>
#include <fstream>
#include <iterator>

namespace hps {

namespace {

constexpr uint32_t DEFAULT_CONNECT_TIMEOUT_MS = 5000;

} // anonymous namespace

CTcpClient::CTcpClient(const std::string& server_ip, uint16_t server_port) :
    server_ip_(server_ip), server_port_(server_port), connect_timeout_ms_(DEFAULT_CONNECT_TIMEOUT_MS) {}

CTcpClient::CTcpClient(uint32_t connect_timeout_ms, const std::string& server_ip, uint16_t server_port) :
    server_ip_(server_ip), server_port_(server_port), connect_timeout_ms_(connect_timeout_ms) {}

CTcpClient::~CTcpClient() {
  disconnect();
}

CTcpClient::CTcpClient(CTcpClient&& other) noexcept :
    client_sockfd_(other.client_sockfd_),
    server_ip_(std::move(other.server_ip_)),
    server_port_(other.server_port_),
    connect_timeout_ms_(other.connect_timeout_ms_),
    read_buf_(std::move(other.read_buf_)) {
  other.client_sockfd_ = -1;
}

CTcpClient& CTcpClient::operator=(CTcpClient&& other) noexcept {
  if (this != &other) {
    disconnect();
    client_sockfd_ = other.client_sockfd_;
    server_ip_ = std::move(other.server_ip_);
    server_port_ = other.server_port_;
    connect_timeout_ms_ = other.connect_timeout_ms_;
    read_buf_ = std::move(other.read_buf_);
    other.client_sockfd_ = -1;
  }
  return *this;
}

bool CTcpClient::send_all(int fd, const char* data, std::size_t size, DurationMs timeout) {
  // N2-H：非阻塞 socket 需处理 EAGAIN/EWOULDBLOCK（poll 等可写后重试）、EINTR（重试）
  std::size_t total = 0;
  while (total < size) {
    ssize_t n = send(fd, std::next(data, static_cast<std::ptrdiff_t>(total)), size - total, 0);
    if (n >= 0) {
      total += static_cast<std::size_t>(n);
      continue;
    }
    // n < 0
    if (errno == EINTR) {
      continue; // 被信号中断，重试
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // 发送缓冲区满，poll 等待可写
      struct pollfd pfd {};
      pfd.fd = fd;
      pfd.events = POLLOUT;
      int ret = poll(&pfd, 1, static_cast<int>(timeout.value));
      if (ret <= 0) {
        return false; // 超时或出错
      }
      continue; // 可写后重试
    }
    return false; // 其他错误
  }
  return true;
}

bool CTcpClient::wait_connected(int fd, DurationMs timeout) {
  struct pollfd pfd {};

  pfd.fd = fd;
  pfd.events = POLLOUT;

  int ret = poll(&pfd, 1, static_cast<int>(timeout.value));
  if (ret <= 0) {
    return false;
  }

  int err = 0;
  socklen_t errlen = sizeof(err);
  return getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) >= 0 && err == 0;
}

bool CTcpClient::wait_readable(int fd, DurationMs timeout) {
  struct pollfd pfd {};

  pfd.fd = fd;
  pfd.events = POLLIN;

  int ret = poll(&pfd, 1, static_cast<int>(timeout.value));
  return ret > 0;
}

bool CTcpClient::connectToServer() {
  disconnect();

  client_sockfd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  if (client_sockfd_ < 0) {
    Logger::_error("创建套接字失败");
    return false;
  }

  std::string port_str = std::to_string(server_port_);

  struct addrinfo hints {};

  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* result = nullptr;
  int ret = getaddrinfo(server_ip_.c_str(), port_str.c_str(), &hints, &result);
  if (ret != 0) {
    Logger::_error("无法解析地址: " + server_ip_ + " - " + gai_strerror(ret));
    disconnect();
    return false;
  }

  int rc = connect(client_sockfd_, result->ai_addr, result->ai_addrlen);
  freeaddrinfo(result);

  if (rc == 0) {
    return true;
  }

  if (errno != EINPROGRESS) {
    Logger::_error("连接失败: " + server_ip_ + ":" + std::to_string(server_port_));
    disconnect();
    return false;
  }

  if (!wait_connected(client_sockfd_, DurationMs{connect_timeout_ms_})) {
    Logger::_error("连接超时: " + server_ip_ + ":" + std::to_string(server_port_));
    disconnect();
    return false;
  }

  return true;
}

void CTcpClient::disconnect() {
  if (client_sockfd_ >= 0) {
    close(client_sockfd_);
    client_sockfd_ = -1;
  }
  read_buf_.clear();
}

bool CTcpClient::sendMessage(const std::string& message) const {
  if (client_sockfd_ < 0) {
    Logger::_error("未连接到服务器");
    return false;
  }
  return send_all(client_sockfd_, message.data(), message.size(), DurationMs{connect_timeout_ms_});
}

bool CTcpClient::receiveMessage(std::string& message, ReadMode mode, uint32_t read_timeout_ms) {
  if (client_sockfd_ < 0) {
    return false;
  }
  if (mode == ReadMode::Line) {
    return read_line(message);
  }
  return read_raw(message, read_timeout_ms);
}

bool CTcpClient::recv_into_buffer(DurationMs timeout) {
  if (!wait_readable(client_sockfd_, timeout)) {
    return false;
  }

  std::array<char, 4096> buf;
  ssize_t n = recv(client_sockfd_, buf.data(), buf.size(), 0);
  if (n > 0) {
    read_buf_.append(buf.data(), static_cast<std::size_t>(n));
    return true;
  }

  if (n == 0) {
    peer_closed_ = true;
    disconnect();
  }
  return false;
}

bool CTcpClient::read_line(std::string& line) {
  while (true) {
    auto pos = read_buf_.find('\n');
    if (pos != std::string::npos) {
      line.assign(read_buf_, 0, pos);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      read_buf_.erase(0, pos + 1);
      return true;
    }
    if (!recv_into_buffer(DurationMs{connect_timeout_ms_})) {
      if (client_sockfd_ < 0) {
        return false;
      }
      if (!read_buf_.empty()) {
        line = std::move(read_buf_);
        read_buf_.clear();
        return true;
      }
      return false;
    }
  }
}

bool CTcpClient::read_raw(std::string& data, uint32_t timeout_ms) {
  data = std::move(read_buf_);
  read_buf_.clear();

  while (wait_readable(client_sockfd_, DurationMs{0})) {
    std::array<char, 4096> buf;
    ssize_t n = recv(client_sockfd_, buf.data(), buf.size(), 0);
    if (n <= 0) {
      if (n == 0) {
        peer_closed_ = true;
        disconnect();
      }
      return !data.empty();
    }
    data.append(buf.data(), static_cast<std::size_t>(n));
  }

  if (!recv_into_buffer(DurationMs{timeout_ms})) {
    if (client_sockfd_ < 0) {
      return !data.empty();
    }
    return !data.empty();
  }

  while (true) {
    data += read_buf_;
    read_buf_.clear();
    if (!wait_readable(client_sockfd_, DurationMs{0})) {
      break;
    }
    if (!recv_into_buffer(DurationMs{0})) {
      break;
    }
  }
  return true;
}

bool CTcpClient::sendFile(const std::string& file_path) const {
  if (client_sockfd_ < 0) {
    Logger::_error("未连接到服务器");
    return false;
  }

  std::array<char, 4096> buf;
  std::ifstream file(file_path, std::ios::binary);
  if (!file) {
    Logger::_error("无法打开文件: " + file_path);
    return false;
  }

  while (file.read(buf.data(), buf.size())) {
    if (!send_all(client_sockfd_, buf.data(), buf.size(), DurationMs{connect_timeout_ms_})) {
      Logger::_error("发送文件数据失败");
      return false;
    }
  }

  std::streamsize remaining = file.gcount();
  if (remaining > 0) {
    if (!send_all(client_sockfd_, buf.data(), static_cast<std::size_t>(remaining), DurationMs{connect_timeout_ms_})) {
      Logger::_error("发送文件数据失败");
      return false;
    }
  }
  return true;
}

bool CTcpClient::sendFile(const std::string& data, std::size_t size) const {
  if (client_sockfd_ < 0) {
    Logger::_error("未连接到服务器");
    return false;
  }
  return send_all(client_sockfd_, data.data(), size, DurationMs{connect_timeout_ms_});
}

} // namespace hps
