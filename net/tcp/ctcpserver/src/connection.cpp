#include "connection.h"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace hps {

Connection::Connection(int fd, const sockaddr_in& addr) : fd_(fd), addr_(addr), last_active_(Clock::now()) {}

Connection::~Connection() {
  if (state_ != State::CLOSED) {
    close();
  }
}

std::string Connection::client_ip() const {
  return inet_ntoa(addr_.sin_addr);
}

uint16_t Connection::client_port() const {
  return ntohs(addr_.sin_port);
}

ssize_t Connection::read_from_fd() {
  if (state_ == State::CLOSED || fd_ < 0) {
    return -1;
  }

  ssize_t total_read = 0;
  std::array<char, 65536> buf{};

  while (true) {
    ssize_t n = ::read(fd_, buf.data(), buf.size());
    if (n > 0) {
      read_buffer_.append(buf.data(), static_cast<size_t>(n));
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
  if (state_ == State::CLOSED || fd_ < 0) {
    return -1;
  }

  ssize_t total_written = 0;

  while (!write_buffer_.empty()) {
    ssize_t n = ::write(fd_, write_buffer_.data(), write_buffer_.size());
    if (n > 0) {
      write_buffer_.erase(0, static_cast<size_t>(n));
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
