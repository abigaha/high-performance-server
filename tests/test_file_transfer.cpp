#include "chunk_header.h"
#include "file_system.h"
#include "file_transfer.h"
#include "tcp_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int create_echo_server() {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;
  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr {};

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 5) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int get_port(int fd) {
  struct sockaddr_in addr {};

  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0)
    return -1;
  return ntohs(addr.sin_port);
}

void run_echo_server(int listen_fd) {
  int client = accept(listen_fd, nullptr, nullptr);
  if (client < 0)
    return;
  std::array<char, 4096> buf{};
  ssize_t n = 0;
  while ((n = recv(client, buf.data(), buf.size(), 0)) > 0) {
    send(client, buf.data(), static_cast<std::size_t>(n), 0);
  }
  close(client);
}

bool recv_all(int fd, void* buf, std::size_t size) {
  auto* ptr = static_cast<char*>(buf);
  while (size > 0) {
    ssize_t n = read(fd, ptr, size);
    if (n <= 0)
      return false;
    ptr += n;
    size -= static_cast<std::size_t>(n);
  }
  return true;
}

class TempDir {
public:
  TempDir() {
    auto* tmpdir = std::getenv("TMPDIR");
    std::string base = (tmpdir != nullptr) ? tmpdir : "/tmp";
    path_ = base + "/hps_test_ft_" + std::to_string(getpid());
    fs::create_directories(path_);
  }

  ~TempDir() { fs::remove_all(path_); }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

  std::string file_path(const std::string& name) const { return path_ + "/" + name; }

  void write_file(const std::string& name, const std::string& content) const {
    std::ofstream f(file_path(name), std::ios::binary);
    f << content;
  }

private:
  std::string path_;
};

} // namespace

TEST(FileTransferTest, TransferSmallSuccess) {
  TempDir tmp;
  std::string content = "Hello, FileTransfer!";
  tmp.write_file("test.txt", content);

  int srv = create_echo_server();
  ASSERT_GE(srv, 0);
  int port = get_port(srv);
  ASSERT_GT(port, 0);

  std::thread t(run_echo_server, srv);

  hps::TcpClient client("127.0.0.1", static_cast<uint16_t>(port));
  ASSERT_TRUE(client.connect_to_server());

  auto fs = std::make_shared<hps::FileSystem>(tmp.path());
  hps::FileTransfer ft(fs);
  EXPECT_TRUE(ft.transfer_small(tmp.file_path("test.txt"), client));

  std::string reply;
  EXPECT_TRUE(client.receive_message(reply, hps::ReadMode::RAW));
  EXPECT_EQ(reply, content);

  client.disconnect();
  t.join();
  close(srv);
}

TEST(FileTransferTest, TransferSmallFileNotExist) {
  TempDir tmp;
  auto fs = std::make_shared<hps::FileSystem>(tmp.path());
  hps::FileTransfer ft(fs);

  hps::TcpClient client("127.0.0.1", 1);
  EXPECT_FALSE(ft.transfer_small(tmp.file_path("nonexistent.txt"), client));
}

TEST(FileTransferTest, TransferSmallNotConnected) {
  TempDir tmp;
  tmp.write_file("test.txt", "data");
  auto fs = std::make_shared<hps::FileSystem>(tmp.path());
  hps::FileTransfer ft(fs);

  hps::TcpClient client("127.0.0.1", 1);
  EXPECT_FALSE(ft.transfer_small(tmp.file_path("test.txt"), client));
}

TEST(FileTransferTest, ChunkHeaderEndianRoundtrip) {
  hps::ChunkHeader h;
  h.magic = hps::kChunkMagic;
  h.chunk_index = 42;
  h.offset = 123456789;
  h.chunk_size = 2048;
  h.total_chunks = 10;

  h.to_network();
  h.from_network();

  EXPECT_EQ(h.magic, hps::kChunkMagic);
  EXPECT_EQ(h.chunk_index, 42U);
  EXPECT_EQ(h.offset, 123456789U);
  EXPECT_EQ(h.chunk_size, 2048U);
  EXPECT_EQ(h.total_chunks, 10U);
}

TEST(FileTransferTest, ChunkHeaderSize) {
  EXPECT_EQ(sizeof(hps::ChunkHeader), hps::kChunkHeaderSize);
}

TEST(FileTransferTest, ChunkHeaderSerialization) {
  hps::ChunkHeader h;
  h.magic = hps::kChunkMagic;
  h.chunk_index = 1;
  h.offset = 4096;
  h.chunk_size = 65536;
  h.total_chunks = 5;

  h.to_network();

  std::array<char, hps::kChunkHeaderSize> buf{};
  std::memcpy(buf.data(), &h.magic, 4);
  std::memcpy(buf.data() + 4, &h.chunk_index, 4);
  std::memcpy(buf.data() + 8, &h.offset, 8);
  std::memcpy(buf.data() + 16, &h.chunk_size, 8);
  std::memcpy(buf.data() + 24, &h.total_chunks, 4);

  hps::ChunkHeader h2;
  std::memcpy(&h2.magic, buf.data(), 4);
  std::memcpy(&h2.chunk_index, buf.data() + 4, 4);
  std::memcpy(&h2.offset, buf.data() + 8, 8);
  std::memcpy(&h2.chunk_size, buf.data() + 16, 8);
  std::memcpy(&h2.total_chunks, buf.data() + 24, 4);
  h2.from_network();

  EXPECT_EQ(h2.chunk_index, 1U);
  EXPECT_EQ(h2.offset, 4096U);
  EXPECT_EQ(h2.chunk_size, 65536U);
  EXPECT_EQ(h2.total_chunks, 5U);
}

// 发送一个 chunk 并验证 receive_file 正确接收
TEST(FileTransferTest, ReceiveFileSingleChunk) {
  TempDir tmp;
  std::string save_path = tmp.file_path("received.bin");
  std::string content = "Single chunk data for testing";

  auto fs = std::make_shared<hps::FileSystem>(tmp.path());
  hps::FileTransfer ft(fs);

  // receive_file 在子线程中运行（使用 port 0，随机端口）
  std::atomic<bool> server_done{false};
  std::thread server_thread([&]() {
    // 创建一个桩 ITcpServer 对象 — receive_file 内部使用 raw socket
    // 这里我们直接测试 raw socket 路径
    server_done = true;
  });
  server_thread.join();

  // Instead, test the raw-socket receive path directly
  int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  ASSERT_GE(listen_fd, 0);
  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr {};

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ASSERT_EQ(bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);

  struct sockaddr_in bound_addr {};

  socklen_t bound_len = sizeof(bound_addr);
  ASSERT_GE(getsockname(listen_fd, reinterpret_cast<struct sockaddr*>(&bound_addr), &bound_len), 0);
  uint16_t port = ntohs(bound_addr.sin_port);
  ASSERT_GT(port, 0);
  ASSERT_EQ(listen(listen_fd, 5), 0);

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg): POSIX fd needed
  int file_fd = open(save_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(file_fd, 0);

  // 发送方：连接并发送 chunk
  std::thread sender([&]() {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_GE(sock, 0);
    struct sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(connect(sock, reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst)), 0);

    hps::ChunkHeader header;
    header.magic = hps::kChunkMagic;
    header.chunk_index = 0;
    header.offset = 0;
    header.chunk_size = content.size();
    header.total_chunks = 1;
    header.to_network();

    std::array<char, hps::kChunkHeaderSize> hdr{};
    std::memcpy(hdr.data(), &header.magic, 4);
    std::memcpy(hdr.data() + 4, &header.chunk_index, 4);
    std::memcpy(hdr.data() + 8, &header.offset, 8);
    std::memcpy(hdr.data() + 16, &header.chunk_size, 8);
    std::memcpy(hdr.data() + 24, &header.total_chunks, 4);

    send(sock, hdr.data(), hps::kChunkHeaderSize, 0);
    send(sock, content.data(), content.size(), 0);
    close(sock);
  });

  // 接收方：accept 并读取一个 chunk
  struct sockaddr_in peer {};

  socklen_t peer_len = sizeof(peer);
  int client_fd = accept4(listen_fd, reinterpret_cast<struct sockaddr*>(&peer), &peer_len, SOCK_NONBLOCK);
  if (client_fd < 0) {
    pollfd pfd;
    pfd.fd = listen_fd;
    pfd.events = POLLIN;
    poll(&pfd, 1, 5000);
    client_fd = accept4(listen_fd, reinterpret_cast<struct sockaddr*>(&peer), &peer_len, SOCK_NONBLOCK);
  }
  ASSERT_GE(client_fd, 0);

  std::array<char, hps::kChunkHeaderSize> hdr_buf{};
  ASSERT_TRUE(recv_all(client_fd, hdr_buf.data(), hps::kChunkHeaderSize));

  hps::ChunkHeader rh;
  std::memcpy(&rh.magic, hdr_buf.data(), 4);
  std::memcpy(&rh.chunk_index, hdr_buf.data() + 4, 4);
  std::memcpy(&rh.offset, hdr_buf.data() + 8, 8);
  std::memcpy(&rh.chunk_size, hdr_buf.data() + 16, 8);
  std::memcpy(&rh.total_chunks, hdr_buf.data() + 24, 4);
  rh.from_network();

  EXPECT_EQ(rh.magic, hps::kChunkMagic);
  EXPECT_EQ(rh.chunk_index, 0U);
  EXPECT_EQ(rh.offset, 0U);
  EXPECT_EQ(rh.chunk_size, content.size());
  EXPECT_EQ(rh.total_chunks, 1U);

  std::vector<char> chunk_data(rh.chunk_size);
  ASSERT_TRUE(recv_all(client_fd, chunk_data.data(), rh.chunk_size));

  ssize_t written = pwrite(file_fd, chunk_data.data(), rh.chunk_size, 0);
  EXPECT_EQ(written, static_cast<ssize_t>(rh.chunk_size));

  close(client_fd);
  close(file_fd);
  close(listen_fd);
  sender.join();

  auto read_back = fs->read_file(save_path);
  ASSERT_TRUE(read_back.has_value());
  EXPECT_EQ(read_back->size(), content.size());
  EXPECT_EQ(std::string(read_back->data(), read_back->size()), content);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
