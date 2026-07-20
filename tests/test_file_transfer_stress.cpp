#include "chunk_header.h"
#include "file_system.h"
#include "file_transfer.h"
#include "tcp_client.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

inline uint64_t htonll(uint64_t v) {
  if (htonl(1) == 1)
    return v;
  return (static_cast<uint64_t>(htonl(static_cast<uint32_t>(v))) << 32) | htonl(static_cast<uint32_t>(v >> 32));
}

inline uint64_t ntohll(uint64_t v) {
  return htonll(v);
}

using namespace hps;

namespace {

class TempDir {
public:
  TempDir() {
    auto* tmpdir = std::getenv("TMPDIR");
    std::string base = (tmpdir != nullptr) ? tmpdir : "/tmp";
    path_ = base + "/hps_test_ft_stress_" + std::to_string(getpid());
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

int create_listener(uint16_t& out_port) {
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
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
    close(fd);
    return -1;
  }
  out_port = ntohs(addr.sin_port);
  if (listen(fd, 10) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

void simple_echo_server(int listen_fd, std::atomic<bool>& done) {
  int client = accept(listen_fd, nullptr, nullptr);
  if (client < 0)
    return;
  char buf[4096];
  ssize_t n = 0;
  while ((n = read(client, buf, sizeof(buf))) > 0) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast, bugprone-unused-return-value)
    if (write(client, buf, static_cast<std::size_t>(n)) < 0) { /* ignore */
    }
  }
  close(client);
  done = true;
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

} // namespace

TEST(FileTransferStressTest, ChunkHeaderSerialize) {
  ChunkHeader h;
  h.magic = kChunkMagic;
  h.chunk_index = 42;
  h.offset = 123456789;
  h.chunk_size = 65536;
  h.total_chunks = 10;

  h.to_network();

  ChunkHeader h2;
  h2.magic = kChunkMagic;
  h2.chunk_index = 0;
  h2.offset = 0;
  h2.chunk_size = 0;
  h2.total_chunks = 0;

  std::memcpy(&h2, &h, kChunkHeaderSize);
  h2.from_network();

  EXPECT_EQ(h2.magic, kChunkMagic);
  EXPECT_EQ(h2.chunk_index, 42U);
  EXPECT_EQ(h2.offset, 123456789U);
  EXPECT_EQ(h2.chunk_size, 65536U);
  EXPECT_EQ(h2.total_chunks, 10U);
}

TEST(FileTransferStressTest, ChunkHeaderNetworkByteOrder) {
  ChunkHeader h;
  h.magic = kChunkMagic;
  h.chunk_index = 1;
  h.offset = 0x0102030405060708ULL;
  h.chunk_size = 0x1122334455667788ULL;
  h.total_chunks = 5;

  h.to_network();

  EXPECT_EQ(ntohl(h.chunk_index), 1U);
  EXPECT_EQ(ntohll(h.offset), 0x0102030405060708ULL);
  EXPECT_EQ(ntohll(h.chunk_size), 0x1122334455667788ULL);
  EXPECT_EQ(ntohl(h.total_chunks), 5U);

  h.from_network();

  EXPECT_EQ(h.chunk_index, 1U);
  EXPECT_EQ(h.offset, 0x0102030405060708ULL);
  EXPECT_EQ(h.chunk_size, 0x1122334455667788ULL);
  EXPECT_EQ(h.total_chunks, 5U);
}

TEST(FileTransferStressTest, ChunkHeaderMagicCheck) {
  EXPECT_EQ(kChunkMagic, 0x48505346U);

  ChunkHeader h;
  h.magic = kChunkMagic;
  h.chunk_index = 0;
  h.offset = 0;
  h.chunk_size = 1024;
  h.total_chunks = 1;

  h.to_network();

  uint32_t net_magic = 0;
  std::memcpy(&net_magic, &h.magic, sizeof(net_magic));
  EXPECT_EQ(ntohl(net_magic), kChunkMagic);
}

TEST(FileTransferStressTest, TransferSmallFileInProcess) {
  TempDir tmp;
  std::string content = "Small file transfer test data";
  tmp.write_file("test.txt", content);

  uint16_t port = 0;
  int srv_fd = create_listener(port);
  ASSERT_GE(srv_fd, 0);
  ASSERT_GT(port, 0);

  std::atomic<bool> server_done{false};
  std::thread server_thread(simple_echo_server, srv_fd, std::ref(server_done));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  TcpClient client("127.0.0.1", port);
  ASSERT_TRUE(client.connect_to_server());

  auto fs_ptr = std::make_shared<FileSystem>(tmp.path());
  FileTransfer ft(fs_ptr);
  EXPECT_TRUE(ft.transfer_small(tmp.file_path("test.txt"), client));

  client.disconnect();
  server_thread.join();
  close(srv_fd);
}

TEST(FileTransferStressTest, ConcurrentTransfers) {
  TempDir tmp;
  auto fs_ptr = std::make_shared<FileSystem>(tmp.path());

  constexpr int kNumTransfers = 4;
  std::vector<uint16_t> ports(kNumTransfers);
  std::vector<int> srv_fds(kNumTransfers);
  std::vector<std::thread> server_threads;
  std::vector<std::atomic<bool>> server_dones(kNumTransfers);

  for (int i = 0; i < kNumTransfers; ++i) {
    server_dones[i] = false;
    srv_fds[i] = create_listener(ports[i]);
    ASSERT_GE(srv_fds[i], 0);
    ASSERT_GT(ports[i], 0);
    std::string content = "Concurrent transfer " + std::to_string(i);
    tmp.write_file("concurrent_" + std::to_string(i) + ".txt", content);
    server_threads.emplace_back(simple_echo_server, srv_fds[i], std::ref(server_dones[i]));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::vector<std::thread> client_threads;
  std::atomic<int> success_count{0};

  for (int i = 0; i < kNumTransfers; ++i) {
    client_threads.emplace_back([&, i]() {
      TcpClient client("127.0.0.1", ports[i]);
      if (client.connect_to_server()) {
        FileTransfer ft(fs_ptr);
        if (ft.transfer_small(tmp.file_path("concurrent_" + std::to_string(i) + ".txt"), client)) {
          success_count.fetch_add(1, std::memory_order_relaxed);
        }
        client.disconnect();
      }
    });
  }

  for (auto& th : client_threads) {
    th.join();
  }
  for (auto& th : server_threads) {
    th.join();
  }
  for (int fd : srv_fds) {
    close(fd);
  }

  EXPECT_EQ(success_count.load(), kNumTransfers);
}

TEST(FileTransferStressTest, TransferConnectionDrop) {
  TempDir tmp;
  std::string content = "Connection drop test data";
  tmp.write_file("drop_test.txt", content);

  int srv_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_GE(srv_fd, 0);
  int opt = 1;
  setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr {};

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);
  ASSERT_EQ(bind(srv_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);
  socklen_t len = sizeof(addr);
  ASSERT_EQ(getsockname(srv_fd, reinterpret_cast<struct sockaddr*>(&addr), &len), 0);
  uint16_t port = ntohs(addr.sin_port);
  ASSERT_GT(port, 0);
  ASSERT_EQ(listen(srv_fd, 1), 0);

  std::thread acceptor([srv_fd]() {
    int client = accept(srv_fd, nullptr, nullptr);
    if (client >= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      close(client);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  {
    TcpClient client("127.0.0.1", port);
    if (client.connect_to_server()) {
      auto fs_ptr = std::make_shared<FileSystem>(tmp.path());
      FileTransfer ft(fs_ptr);
      ft.transfer_small(tmp.file_path("drop_test.txt"), client);
    }
  }

  acceptor.join();
  close(srv_fd);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
