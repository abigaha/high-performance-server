#include "connection.h"
#include "ssl_context.h"
#include "tcp_client.h"
#include "tcp_server.h"
#include "thread_pool.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hps {
namespace {
std::string g_cert_dir;

std::string cert_path(const std::string& name) {
  return g_cert_dir + "/" + name;
}

void ensure_certs() {
  g_cert_dir = "build/certs";
  ::mkdir("build/certs", 0755);
  std::string cert_file = cert_path("cert.pem");

  struct stat st {};

  if (::stat(cert_file.c_str(), &st) != 0) {
    std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + cert_path("key.pem") + " -out " +
                      cert_path("cert.pem") + " -days 3650 -nodes -subj '/CN=localhost/O=hps/C=CN' 2>/dev/null";
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "证书生成失败";
    ASSERT_EQ(::stat(cert_path("cert.pem").c_str(), &st), 0);
    ASSERT_EQ(::stat(cert_path("key.pem").c_str(), &st), 0);
  }
}

// SSL 客户端辅助
struct SslClient {
  int fd{-1};
  SSL* ssl{nullptr};
  SSL_CTX* ctx{nullptr};

  bool connect_to(uint16_t port) {
    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr)
      return false;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
      return false;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(fd);
      fd = -1;
      return false;
    }

    ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_connect(ssl);
    return true;
  }

  ssize_t read(void* buf, size_t len) const { return SSL_read(ssl, buf, static_cast<int>(len)); }

  ssize_t write(const void* buf, size_t len) const { return SSL_write(ssl, buf, static_cast<int>(len)); }

  ~SslClient() {
    if (ssl != nullptr) {
      SSL_shutdown(ssl);
      SSL_free(ssl);
    }
    if (ctx != nullptr)
      SSL_CTX_free(ctx);
    if (fd >= 0)
      ::close(fd);
  }
};

// 原始 TCP 客户端辅助
int raw_connect(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);

  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

} // namespace

// ========== T1: SslContext 创建与销毁 ==========
TEST(SslContextTest, CreateDestroy) {
  ensure_certs();
  SslConfig cfg;
  cfg.cert_file = cert_path("cert.pem");
  cfg.key_file = cert_path("key.pem");
  cfg.enabled = true;
  EXPECT_NO_THROW({ SslContext ctx(cfg); });
}

// ========== T2: 无效证书抛异常 ==========
TEST(SslContextTest, InvalidCertThrows) {
  SslConfig cfg;
  cfg.cert_file = "/nonexistent/cert.pem";
  cfg.key_file = "/nonexistent/key.pem";
  cfg.enabled = true;
  EXPECT_THROW({ SslContext ctx(cfg); }, std::runtime_error);
}

// ========== T3: create_ssl 返回非空 ==========
TEST(SslContextTest, CreateSsl) {
  ensure_certs();
  SslConfig cfg;
  cfg.cert_file = cert_path("cert.pem");
  cfg.key_file = cert_path("key.pem");
  cfg.enabled = true;
  SslContext ctx(cfg);
  ssl_st* ssl = ctx.create_ssl();
  ASSERT_NE(ssl, nullptr);
  ctx.shutdown_and_free(ssl);
}

// ========== T4: TLS 握手 + 加密通信 ==========
TEST(SslServerTest, HandshakeAndEcho) {
  ensure_certs();
  TcpServer::Config srv_cfg;
  srv_cfg.port = 0;
  srv_cfg.thread_count = 2;
  srv_cfg.epoll_timeout_ms = 50;
  srv_cfg.ssl_config.cert_file = cert_path("cert.pem");
  srv_cfg.ssl_config.key_file = cert_path("key.pem");
  srv_cfg.ssl_config.enabled = true;

  std::atomic<bool> handler_called{false};
  TcpServer server(srv_cfg);
  server.set_handler([&](std::shared_ptr<Connection> conn) {
    handler_called = true;
    std::lock_guard<std::mutex> wlock(conn->write_mutex());
    conn->write_buffer() = conn->read_buffer();
  });
  ASSERT_TRUE(server.init());
  uint16_t port = server.actual_port();
  ASSERT_GT(port, 0);

  std::thread t([&server]() { server.start(); });
  t.detach();
  while (!server.is_running()) {
    std::this_thread::yield();
  }

  SslClient client;
  ASSERT_TRUE(client.connect_to(port));

  const std::string msg = "hello-tls";
  ssize_t n = client.write(msg.data(), msg.size());
  ASSERT_EQ(n, static_cast<ssize_t>(msg.size()));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_TRUE(handler_called);
}

// ========== T5: SSL 加密读写 ==========
TEST(SslServerTest, EncryptedReadWrite) {
  ensure_certs();
  TcpServer::Config srv_cfg;
  srv_cfg.port = 0;
  srv_cfg.thread_count = 2;
  srv_cfg.epoll_timeout_ms = 50;
  srv_cfg.ssl_config.cert_file = cert_path("cert.pem");
  srv_cfg.ssl_config.key_file = cert_path("key.pem");
  srv_cfg.ssl_config.enabled = true;

  std::string echoed;
  std::atomic<bool> done{false};
  TcpServer server(srv_cfg);
  server.set_handler([&](std::shared_ptr<Connection> conn) {
    echoed = conn->read_buffer();
    {
      std::lock_guard<std::mutex> wlock(conn->write_mutex());
      conn->write_buffer() = echoed;
    }
    done = true;
  });
  ASSERT_TRUE(server.init());
  uint16_t port = server.actual_port();

  std::thread t([&server]() { server.start(); });
  t.detach();
  while (!server.is_running()) {
    std::this_thread::yield();
  }

  SslClient client;
  ASSERT_TRUE(client.connect_to(port));

  const std::string msg = "encrypted-echo-test";
  client.write(msg.data(), msg.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_TRUE(done);
  EXPECT_EQ(echoed, msg);
}

// ========== T6: 双模式——明文 HTTP ==========
TEST(SslServerTest, DualModePlainHttp) {
  ensure_certs();
  TcpServer::Config srv_cfg;
  srv_cfg.port = 0;
  srv_cfg.thread_count = 2;
  srv_cfg.epoll_timeout_ms = 50;
  srv_cfg.ssl_config.cert_file = cert_path("cert.pem");
  srv_cfg.ssl_config.key_file = cert_path("key.pem");
  srv_cfg.ssl_config.enabled = true;

  std::atomic<bool> plain_called{false};
  TcpServer server(srv_cfg);
  server.set_handler([&](std::shared_ptr<Connection> conn) {
    plain_called = true;
    std::lock_guard<std::mutex> wlock(conn->write_mutex());
    conn->write_buffer() = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
  });
  ASSERT_TRUE(server.init());
  uint16_t port = server.actual_port();

  std::thread t([&server]() { server.start(); });
  t.detach();
  while (!server.is_running()) {
    std::this_thread::yield();
  }

  int fd = raw_connect(port);
  ASSERT_GE(fd, 0);
  const std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  [[maybe_unused]] auto nw = ::write(fd, req.data(), req.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ::close(fd);
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_TRUE(plain_called);
}

// ========== T7: 双模式——HTTPS 请求 ==========
TEST(SslServerTest, DualModeHttpsRequest) {
  ensure_certs();
  TcpServer::Config srv_cfg;
  srv_cfg.port = 0;
  srv_cfg.thread_count = 2;
  srv_cfg.epoll_timeout_ms = 50;
  srv_cfg.ssl_config.cert_file = cert_path("cert.pem");
  srv_cfg.ssl_config.key_file = cert_path("key.pem");
  srv_cfg.ssl_config.enabled = true;

  std::atomic<bool> ssl_called{false};
  TcpServer server(srv_cfg);
  server.set_handler([&](std::shared_ptr<Connection> /*conn*/) { ssl_called = true; });
  ASSERT_TRUE(server.init());
  uint16_t port = server.actual_port();

  std::thread t([&server]() { server.start(); });
  t.detach();
  while (!server.is_running()) {
    std::this_thread::yield();
  }

  SslClient client;
  ASSERT_TRUE(client.connect_to(port));
  const std::string msg = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
  client.write(msg.data(), msg.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_TRUE(ssl_called);
}

// ========== T8: Connection SSL 清理 ==========
TEST(SslServerTest, ConnectionCleanup) {
  ensure_certs();
  SslConfig cfg;
  cfg.cert_file = cert_path("cert.pem");
  cfg.key_file = cert_path("key.pem");
  cfg.enabled = true;
  SslContext ctx(cfg);
  ssl_st* ssl = ctx.create_ssl();

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;

  {
    auto conn = std::make_shared<Connection>(0, addr);
    conn->set_ssl(ssl);
    conn->set_ssl_state(Connection::SslState::ESTABLISHED);
    // conn 析构时调用 close → SSL_shutdown + SSL_free
  }

  SUCCEED();
}

// ========== T9: 未启用 SSL 时零开销 ==========
TEST(SslServerTest, SslDisabledNoContext) {
  TcpServer::Config srv_cfg;
  srv_cfg.port = 0;
  srv_cfg.ssl_config.enabled = false;

  TcpServer server(srv_cfg);
  ASSERT_TRUE(server.init());
  EXPECT_GT(server.actual_port(), 0);
  server.stop();
}

} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
