#include "connection.h"

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

  ssize_t total_read = 0;
  // N9-L：64KB 缓冲改 thread_local 避免深栈（ASan 下栈开销更大）
  static thread_local std::vector<char> t_buf;
  if (t_buf.size() < 65536) {
    t_buf.resize(65536);
  }

  while (true) {
    ssize_t n = ::read(fd_, t_buf.data(), t_buf.size());
    if (n > 0) {
      read_buffer_.append(t_buf.data(), static_cast<size_t>(n));
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

  update_active();
  return total_read;
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

  ssize_t total_written = 0;

  // N5-M：用偏移量游标避免 erase(0,n) 的 O(n) 内存移动
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
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void Connection::consume_read_buffer(size_t bytes) {
  if (bytes >= read_buffer_.size()) {
    read_buffer_.clear();
  } else {
    read_buffer_.erase(0, bytes);
  }
  update_active();
}

} // namespace hps
