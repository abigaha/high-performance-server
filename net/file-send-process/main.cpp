#include "chunk_header.h"
#include "logger.h"
#include "tcp_client.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct SendTask {
  std::size_t index;
  std::size_t offset;
  std::size_t size;
  std::string peer_ip;
  uint16_t peer_port;
  std::size_t total_chunks;
};

bool send_all(int fd, const char* data, std::size_t size) {
  while (size > 0) {
    ssize_t n = write(fd, data, size);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        poll(&pfd, 1, 30000);
        continue;
      }
      return false;
    }
    data += n;
    size -= static_cast<std::size_t>(n);
  }
  return true;
}

void send_chunk(const SendTask& task, int file_fd) {
  hps::TcpClient client(task.peer_ip, task.peer_port);
  if (!client.connect_to_server()) {
    hps::Logger::_error("send-process: connect failed for chunk " + std::to_string(task.index));
    return;
  }

  hps::ChunkHeader header;
  header.magic = hps::kChunkMagic;
  header.chunk_index = static_cast<uint32_t>(task.index);
  header.offset = static_cast<uint64_t>(task.offset);
  header.chunk_size = static_cast<uint64_t>(task.size);
  header.total_chunks = static_cast<uint32_t>(task.total_chunks);

  hps::Logger::_info("send-process: sending chunk " + std::to_string(task.index + 1) + "/" +
                     std::to_string(task.total_chunks) + " (offset=" + std::to_string(task.offset) +
                     ", size=" + std::to_string(task.size) + ")");

  int raw_fd = -1;
  {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
      return;

    struct sockaddr_in addr {};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(task.peer_port);
    inet_pton(AF_INET, task.peer_ip.c_str(), &addr.sin_addr);

    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      close(sock);
      return;
    }
    raw_fd = sock;
  }

  header.to_network();
  std::array<char, hps::kChunkHeaderSize> header_buf{};
  std::memcpy(header_buf.data(), &header.magic, 4);
  std::memcpy(header_buf.data() + 4, &header.chunk_index, 4);
  std::memcpy(header_buf.data() + 8, &header.offset, 8);
  std::memcpy(header_buf.data() + 16, &header.chunk_size, 8);
  std::memcpy(header_buf.data() + 24, &header.total_chunks, 4);

  if (!send_all(raw_fd, header_buf.data(), hps::kChunkHeaderSize)) {
    hps::Logger::_error("send-process: failed to send header for chunk " + std::to_string(task.index));
    close(raw_fd);
    return;
  }

  std::vector<char> buf(task.size);
  ssize_t nread = pread(file_fd, buf.data(), task.size, static_cast<off_t>(task.offset));
  if (nread < 0 || static_cast<std::size_t>(nread) != task.size) {
    hps::Logger::_error("send-process: pread failed for chunk " + std::to_string(task.index));
    close(raw_fd);
    return;
  }

  if (!send_all(raw_fd, buf.data(), buf.size())) {
    hps::Logger::_error("send-process: failed to send data for chunk " + std::to_string(task.index));
    close(raw_fd);
    return;
  }

  close(raw_fd);
  hps::Logger::_info("send-process: chunk " + std::to_string(task.index + 1) + "/" + std::to_string(task.total_chunks) +
                     " sent successfully");
}

} // anonymous namespace

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "exit")
      break;

    std::istringstream iss(line);
    std::size_t total_size = 0;
    std::string path;
    std::string peer_ip;
    uint16_t peer_port = 0;
    std::size_t total_chunks = 0;

    uint16_t peer_port_tmp = 0;
    if (!(iss >> total_size >> path >> peer_ip >> peer_port_tmp >> total_chunks)) {
      hps::Logger::_error("send-process: invalid input format: " + line);
      continue;
    }
    peer_port = peer_port_tmp;
    std::string log_msg = "send-process: file=" + path + " size=" + std::to_string(total_size);
    log_msg += " chunks=" + std::to_string(total_chunks);
    log_msg += " -> " + peer_ip + ":" + std::to_string(peer_port);
    hps::Logger::_info(log_msg);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg): POSIX fd needed for pread
    int file_fd = open(path.c_str(), O_RDONLY);
    if (file_fd < 0) {
      hps::Logger::_error("send-process: cannot open file: " + path);
      continue;
    }

    std::vector<SendTask> tasks;
    tasks.reserve(total_chunks);
    for (std::size_t i = 0; i < total_chunks; ++i) {
      if (!std::getline(std::cin, line))
        break;

      std::istringstream chunk_ss(line);
      std::size_t index = 0;
      std::size_t offset = 0;
      std::size_t size = 0;

      if (!(chunk_ss >> index >> offset >> size)) {
        hps::Logger::_error("send-process: invalid chunk info: " + line);
        continue;
      }

      tasks.push_back({index, offset, size, peer_ip, peer_port, total_chunks});
    }

    hps::LockFreeThreadPool pool(std::min(tasks.size(), static_cast<std::size_t>(16)));
    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};

    for (const auto& task : tasks) {
      pool.enqueue([&task, file_fd, &success_count, &fail_count]() {
        send_chunk(task, file_fd);
        success_count++;
      });
    }

    pool.wait_for_all_tasks();
    close(file_fd);

    hps::Logger::_info("send-process: completed, success=" + std::to_string(success_count.load()) +
                       " fail=" + std::to_string(fail_count.load()));
  }
  return 0;
}
