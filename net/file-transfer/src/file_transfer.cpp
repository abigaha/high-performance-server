#include "file_transfer.h"

#include "chunk_header.h"
#include "i_file_system.h"
#include "i_tcp_client.h"
#include "i_tcp_server.h"
#include "logger.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <vector>

namespace hps {

FileTransfer::FileTransfer(std::shared_ptr<IFileSystem> fs, std::size_t chunk_size) :
    fs_(std::move(fs)), chunk_size_(chunk_size) {}

bool FileTransfer::transfer_small(const std::string& path, ITcpClient& client) {
  if (!client.is_connected()) {
    Logger::_error("FileTransfer::transfer_small: client not connected");
    return false;
  }
  auto data = fs_->read_file(path);
  if (!data) {
    Logger::_error("FileTransfer::transfer_small: cannot read file: " + path);
    return false;
  }
  return client.send_message(std::string(data->data(), data->size()));
}

bool FileTransfer::transfer_large(const std::string& path, const std::string& peer_ip, uint16_t peer_port) {
  auto chunks = fs_->split_file(path, chunk_size_);
  if (chunks.empty()) {
    Logger::_error("FileTransfer::transfer_large: cannot split file: " + path);
    return false;
  }

  std::size_t total_size = 0;
  for (const auto& c : chunks) {
    total_size += c.size;
  }

  std::ostringstream ss;
  ss << total_size << " " << path << " " << peer_ip << " " << peer_port << " " << chunks.size() << "\n";
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    ss << i << " " << chunks[i].offset << " " << chunks[i].size << "\n";
  }
  std::string input = ss.str();

  std::array<int, 2> pipe_stdin = {-1, -1};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
  if (pipe(pipe_stdin.data()) < 0) {
    Logger::_error("FileTransfer::transfer_large: pipe failed: " + std::string(std::strerror(errno)));
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    Logger::_error("FileTransfer::transfer_large: fork failed: " + std::string(std::strerror(errno)));
    close(pipe_stdin[0]);
    close(pipe_stdin[1]);
    return false;
  }

  if (pid == 0) {
    close(pipe_stdin[1]);
    dup2(pipe_stdin[0], STDIN_FILENO);
    close(pipe_stdin[0]);
    for (int i = 3; i < 64; ++i) {
      close(i);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg): exec required
    execlp("file-send-process", "file-send-process", nullptr);
    Logger::_error("FileTransfer::transfer_large: execlp failed: " + std::string(std::strerror(errno)));
    _exit(1);
  }

  close(pipe_stdin[0]);
  const char* data = input.c_str();
  std::size_t remaining = input.size();
  while (remaining > 0) {
    ssize_t n = write(pipe_stdin[1], data, remaining);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    data += n;
    remaining -= static_cast<std::size_t>(n);
  }
  close(pipe_stdin[1]);

  int status = 0;
  waitpid(pid, &status, 0);

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    Logger::_error("FileTransfer::transfer_large: child process failed, status=" + std::to_string(status));
    return false;
  }
  return true;
}

namespace {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): int& fd vs uint16_t& port, types differ
bool setup_listen_socket(int& listen_fd, uint16_t& port) {
  listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  if (listen_fd < 0)
    return false;

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr {};

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = 0;

  if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(listen_fd);
    return false;
  }

  struct sockaddr_in bound_addr {};

  socklen_t bound_len = sizeof(bound_addr);
  if (getsockname(listen_fd, reinterpret_cast<struct sockaddr*>(&bound_addr), &bound_len) < 0) {
    close(listen_fd);
    return false;
  }
  port = ntohs(bound_addr.sin_port);

  if (listen(listen_fd, 16) < 0) {
    close(listen_fd);
    return false;
  }
  return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): listen vs file fd, semantically distinct
bool accept_and_receive_chunk(int listen_fd, int file_fd) {
  struct sockaddr_in peer_addr {};

  socklen_t peer_len = sizeof(peer_addr);
  int client_fd = accept4(listen_fd, reinterpret_cast<struct sockaddr*>(&peer_addr), &peer_len, SOCK_NONBLOCK);

  if (client_fd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      pollfd pfd;
      pfd.fd = listen_fd;
      pfd.events = POLLIN;
      poll(&pfd, 1, 30000);
      return true;
    }
    return false;
  }

  std::array<char, kChunkHeaderSize> header_buf{};
  std::size_t remaining = kChunkHeaderSize;
  char* hptr = header_buf.data();
  while (remaining > 0) {
    ssize_t n = read(client_fd, hptr, remaining);
    if (n <= 0) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
        pollfd pfd;
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        poll(&pfd, 1, 30000);
        continue;
      }
      close(client_fd);
      return false;
    }
    hptr += n;
    remaining -= static_cast<std::size_t>(n);
  }

  ChunkHeader header;
  std::memcpy(&header.magic, header_buf.data(), 4);
  std::memcpy(&header.chunk_index, header_buf.data() + 4, 4);
  std::memcpy(&header.offset, header_buf.data() + 8, 8);
  std::memcpy(&header.chunk_size, header_buf.data() + 16, 8);
  std::memcpy(&header.total_chunks, header_buf.data() + 24, 4);
  header.from_network();

  if (header.magic != kChunkMagic) {
    Logger::_error("receive: invalid magic");
    close(client_fd);
    return false;
  }

  std::vector<char> chunk_buf(header.chunk_size);
  remaining = header.chunk_size;
  char* dptr = chunk_buf.data();
  while (remaining > 0) {
    ssize_t n = read(client_fd, dptr, remaining);
    if (n <= 0) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
        pollfd pfd;
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        poll(&pfd, 1, 30000);
        continue;
      }
      close(client_fd);
      return false;
    }
    dptr += n;
    remaining -= static_cast<std::size_t>(n);
  }

  ssize_t written = pwrite(file_fd, chunk_buf.data(), header.chunk_size, static_cast<off_t>(header.offset));
  if (written < 0 || static_cast<std::size_t>(written) != header.chunk_size) {
    close(client_fd);
    return false;
  }

  close(client_fd);

  Logger::_info("receive: chunk " + std::to_string(header.chunk_index + 1) + "/" + std::to_string(header.total_chunks));
  return header.chunk_index + 1 >= header.total_chunks;
}

} // anonymous namespace

bool FileTransfer::receive_file(const std::string& save_path, ITcpServer& /*server*/) {
  int listen_fd = -1;
  uint16_t listen_port = 0;
  if (!setup_listen_socket(listen_fd, listen_port)) {
    Logger::_error("FileTransfer::receive_file: socket setup failed");
    return false;
  }

  Logger::_info("FileTransfer::receive_file: listening on port " + std::to_string(listen_port) +
                " for file: " + save_path);

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg): POSIX fd needed for pwrite
  int file_fd = open(save_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (file_fd < 0) {
    Logger::_error("FileTransfer::receive_file: cannot open " + save_path);
    close(listen_fd);
    return false;
  }

  bool all_done = false;
  while (!all_done) {
    bool ok = accept_and_receive_chunk(listen_fd, file_fd);
    if (!ok)
      break;
    all_done = ok;
  }

  close(file_fd);
  close(listen_fd);

  if (!all_done) {
    unlink(save_path.c_str());
    return false;
  }
  return true;
}

bool FileTransfer::recv_all(int fd, void* buf, std::size_t size) {
  auto* ptr = static_cast<char*>(buf);
  while (size > 0) {
    ssize_t n = read(fd, ptr, size);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        poll(&pfd, 1, 30000);
        continue;
      }
      return false;
    }
    if (n == 0)
      return false;
    ptr += n;
    size -= static_cast<std::size_t>(n);
  }
  return true;
}

} // namespace hps
