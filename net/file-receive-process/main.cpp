#include "chunk_header.h"
#include "logger.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ClientContext {
  int fd;
  std::size_t total_chunks;
  std::atomic<int>* done_count;
  int file_fd;
  std::atomic<bool>* failed;
};

void handle_client(ClientContext ctx) {
  std::array<char, hps::kChunkHeaderSize> header_buf{};
  std::size_t remaining = hps::kChunkHeaderSize;
  char* ptr = header_buf.data();

  while (remaining > 0) {
    ssize_t n = read(ctx.fd, ptr, remaining);
    if (n <= 0) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
        pollfd pfd;
        pfd.fd = ctx.fd;
        pfd.events = POLLIN;
        poll(&pfd, 1, 30000);
        continue;
      }
      hps::Logger::_error("receive-process: header read failed");
      *ctx.failed = true;
      close(ctx.fd);
      return;
    }
    ptr += n;
    remaining -= static_cast<std::size_t>(n);
  }

  hps::ChunkHeader header;
  std::memcpy(&header.magic, header_buf.data(), 4);
  std::memcpy(&header.chunk_index, header_buf.data() + 4, 4);
  std::memcpy(&header.offset, header_buf.data() + 8, 8);
  std::memcpy(&header.chunk_size, header_buf.data() + 16, 8);
  std::memcpy(&header.total_chunks, header_buf.data() + 24, 4);
  header.from_network();

  if (header.magic != hps::kChunkMagic) {
    hps::Logger::_error("receive-process: invalid magic from chunk " + std::to_string(header.chunk_index));
    *ctx.failed = true;
    close(ctx.fd);
    return;
  }

  std::vector<char> chunk_buf(header.chunk_size);
  remaining = header.chunk_size;
  ptr = chunk_buf.data();

  while (remaining > 0) {
    ssize_t n = read(ctx.fd, ptr, remaining);
    if (n <= 0) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
        pollfd pfd;
        pfd.fd = ctx.fd;
        pfd.events = POLLIN;
        poll(&pfd, 1, 30000);
        continue;
      }
      hps::Logger::_error("receive-process: chunk data read failed");
      *ctx.failed = true;
      close(ctx.fd);
      return;
    }
    ptr += n;
    remaining -= static_cast<std::size_t>(n);
  }

  ssize_t written = pwrite(ctx.file_fd, chunk_buf.data(), header.chunk_size, static_cast<off_t>(header.offset));
  if (written < 0 || static_cast<std::size_t>(written) != header.chunk_size) {
    hps::Logger::_error("receive-process: pwrite failed for chunk " + std::to_string(header.chunk_index));
    *ctx.failed = true;
    close(ctx.fd);
    return;
  }

  close(ctx.fd);

  int done = ctx.done_count->fetch_add(1) + 1;
  hps::Logger::_info("receive-process: received chunk " + std::to_string(done) + "/" +
                     std::to_string(ctx.total_chunks));
}

bool parse_args(int argc, char** argv, uint16_t& port, std::string& save_path, std::size_t& total_chunks) {
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string key(argv[i]);
    if (key == "--port") {
      port = static_cast<uint16_t>(std::stoi(argv[i + 1]));
    } else if (key == "--save_path") {
      save_path = argv[i + 1];
    } else if (key == "--total_chunks") {
      total_chunks = static_cast<std::size_t>(std::stoul(argv[i + 1]));
    }
  }
  return port != 0 && !save_path.empty() && total_chunks != 0;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
  uint16_t listen_port = 0;
  std::string save_path;
  std::size_t total_chunks = 0;

  if (!parse_args(argc, argv, listen_port, save_path, total_chunks)) {
    hps::Logger::_error("Usage: file-receive-process --port <port> --save_path <path> --total_chunks <N>");
    return 1;
  }

  signal(SIGINT, [](int /*signum*/) { /* exit accept loop via EINTR */ });
  signal(SIGTERM, [](int /*signum*/) { /* exit accept loop via EINTR */ });

  int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  if (listen_fd < 0) {
    hps::Logger::_error("receive-process: socket failed");
    return 1;
  }

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr {};

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(listen_port);

  if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    hps::Logger::_error("receive-process: bind failed on port " + std::to_string(listen_port));
    close(listen_fd);
    return 1;
  }

  if (listen(listen_fd, 16) < 0) {
    hps::Logger::_error("receive-process: listen failed");
    close(listen_fd);
    return 1;
  }

  hps::Logger::_info("receive-process: listening on port " + std::to_string(listen_port) + ", save_path=" + save_path +
                     ", total_chunks=" + std::to_string(total_chunks));

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg): POSIX fd needed for pwrite
  int file_fd = open(save_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (file_fd < 0) {
    hps::Logger::_error("receive-process: cannot open file: " + save_path);
    close(listen_fd);
    return 1;
  }

  std::atomic<int> done_count{0};
  std::atomic<bool> failed{false};
  std::vector<std::thread> threads;

  while ((done_count.load() < static_cast<int>(total_chunks)) && !failed.load()) {
    struct sockaddr_in peer_addr {};

    socklen_t peer_len = sizeof(peer_addr);
    int client_fd = accept4(listen_fd, reinterpret_cast<struct sockaddr*>(&peer_addr), &peer_len, SOCK_NONBLOCK);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        pollfd pfd;
        pfd.fd = listen_fd;
        pfd.events = POLLIN;
        poll(&pfd, 1, 1000);
        continue;
      }
      if (errno == EINTR)
        break;
      hps::Logger::_error("receive-process: accept failed");
      break;
    }

    threads.emplace_back(handle_client, ClientContext{client_fd, total_chunks, &done_count, file_fd, &failed});
  }

  for (auto& t : threads) {
    if (t.joinable())
      t.join();
  }

  close(file_fd);
  close(listen_fd);

  if (failed.load()) {
    unlink(save_path.c_str());
    hps::Logger::_error("receive-process: failed, deleted partial file: " + save_path);
    return 1;
  }

  if (done_count.load() < static_cast<int>(total_chunks)) {
    unlink(save_path.c_str());
    hps::Logger::_error("receive-process: incomplete (" + std::to_string(done_count.load()) + "/" +
                        std::to_string(total_chunks) + "), deleted partial file");
    return 1;
  }

  hps::Logger::_info("receive-process: file received successfully: " + save_path);
  return 0;
}
