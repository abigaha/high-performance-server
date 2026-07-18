#include "connection.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <vector>

namespace hps {

Connection::Connection(int fd, const sockaddr_in& addr) : fd_(fd), addr_(addr), last_active_(Clock::now()) {}

Connection::~Connection() {
  if (state_ != State::CLOSED) {
    close();
  }
}

std::string Connection::client_ip() const {
  std::array<char, static_cast<std::size_t>(INET_ADDRSTRLEN)> buf{};
  inet_ntop(AF_INET, &addr_.sin_addr, buf.data(), buf.size());
  return {buf.data()};
}

uint16_t Connection::client_port() const {
  return ntohs(addr_.sin_port);
}

ssize_t Connection::read_from_fd() {
  if (state_ == State::CLOSED || fd_ < 0) {
    return -1;
  }

  ssize_t total_read = (ssl_ != nullptr && ssl_state_ == SslState::ESTABLISHED) ? ssl_read_from_fd()
                                                                                : plain_read_from_fd();

  update_active();
  return total_read;
}

ssize_t Connection::ssl_read_from_fd() {
  static thread_local std::vector<char> t_buf;
  if (t_buf.size() < 65536) {
    t_buf.resize(65536);
  }
  ssize_t total_read = 0;
  while (true) {
    ssize_t n = SSL_read(static_cast<SSL*>(ssl_), t_buf.data(), static_cast<int>(t_buf.size()));
    if (n > 0) {
      {
        std::lock_guard<std::mutex> lock(read_mutex_);
        read_buffer_.append(t_buf.data(), static_cast<size_t>(n));
      }
      total_read += n;
    } else {
      int err = SSL_get_error(static_cast<SSL*>(ssl_), static_cast<int>(n));
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        break;
      }
      if (err == SSL_ERROR_ZERO_RETURN) {
        if (total_read == 0) {
          return 0;
        }
        break;
      }
      if (total_read == 0) {
        return -1;
      }
      break;
    }
  }
  return total_read;
}

ssize_t Connection::plain_read_from_fd() {
  // N9-L：64KB 缓冲改 thread_local 避免深栈（ASan 下栈开销更大）
  static thread_local std::vector<char> t_buf;
  if (t_buf.size() < 65536) {
    t_buf.resize(65536);
  }
  ssize_t total_read = 0;
  while (true) {
    ssize_t n = ::read(fd_, t_buf.data(), t_buf.size());
    if (n > 0) {
      {
        std::lock_guard<std::mutex> lock(read_mutex_);
        read_buffer_.append(t_buf.data(), static_cast<size_t>(n));
      }
      total_read += n;
    } else if (n == 0) {
      if (total_read == 0) {
        return 0;
      }
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (total_read == 0) {
        return -1;
      }
      break;
    }
  }
  return total_read;
}

ssize_t Connection::ssl_write_to_fd_locked() {
  ssize_t total_written = 0;
  while (write_offset_ < write_buffer_.size()) {
    ssize_t n = SSL_write(static_cast<SSL*>(ssl_),
                          write_buffer_.data() + write_offset_,
                          static_cast<int>(write_buffer_.size() - write_offset_));
    if (n > 0) {
      write_offset_ += static_cast<size_t>(n);
      total_written += n;
    } else {
      int err = SSL_get_error(static_cast<SSL*>(ssl_), static_cast<int>(n));
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        break;
      }
      if (err == SSL_ERROR_ZERO_RETURN) {
        break;
      }
      if (total_written == 0) {
        return -1;
      }
      break;
    }
  }
  return total_written;
}

ssize_t Connection::plain_write_to_fd_locked() {
  // N5-M：用偏移量游标避免 erase(0,n) 的 O(n) 内存移动
  ssize_t total_written = 0;
  while (write_offset_ < write_buffer_.size()) {
    ssize_t n = ::write(fd_, write_buffer_.data() + write_offset_, write_buffer_.size() - write_offset_);
    if (n > 0) {
      write_offset_ += static_cast<size_t>(n);
      total_written += n;
    } else if (n == 0) {
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (total_written == 0) {
        return -1;
      }
      break;
    }
  }
  return total_written;
}

ssize_t Connection::write_to_fd() {
  // N1-H：加锁保护 write_buffer_ 跨线程并发
  std::lock_guard lock(write_mutex_);
  return write_to_fd_locked();
}

ssize_t Connection::write_to_fd_locked() {
  if (state_ == State::CLOSED || fd_ < 0) {
    return -1;
  }

  ssize_t total_written = (ssl_ != nullptr && ssl_state_ == SslState::ESTABLISHED) ? ssl_write_to_fd_locked()
                                                                                   : plain_write_to_fd_locked();

  // 已全部发送：compact 缓冲区
  if (write_offset_ >= write_buffer_.size()) {
    write_buffer_.clear();
    write_offset_ = 0;
  } else if (write_offset_ > write_buffer_.size() / 2) {
    // 偏移超过一半，compact 一次避免无限增长
    write_buffer_.erase(0, write_offset_);
    write_offset_ = 0;
  }

  update_active();
  return total_written;
}

void Connection::close() {
  if (state_ == State::CLOSED) {
    return;
  }
  state_ = State::CLOSED;
  if (ssl_ != nullptr) {
    SSL_shutdown(static_cast<SSL*>(ssl_));
    SSL_free(static_cast<SSL*>(ssl_));
    ssl_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void Connection::consume_read_buffer(size_t bytes) {
  std::lock_guard<std::mutex> lock(read_mutex_);
  consume_read_buffer_locked(bytes);
}

void Connection::consume_read_buffer_locked(size_t bytes) {
  if (bytes >= read_buffer_.size()) {
    read_buffer_.clear();
  } else {
    read_buffer_.erase(0, bytes);
  }
  update_active();
}

// ==================== 协程可等待对象实现 ====================
// F6：当前为简单实现（同步读/写 + 立即 resume），后续可集成 epoll 变为真异步

ReadAwaiter Connection::await_read() noexcept {
  return ReadAwaiter{this};
}

WriteAwaiter Connection::await_write() noexcept {
  return WriteAwaiter{this};
}

void ReadAwaiter::await_suspend(std::coroutine_handle<> handle) {
  result = conn->read_from_fd();
  handle.resume();
}

void WriteAwaiter::await_suspend(std::coroutine_handle<> handle) {
  result = conn->write_to_fd();
  handle.resume();
}

} // namespace hps
