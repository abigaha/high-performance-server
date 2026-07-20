#include "ssl_context.h"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <string>

using namespace hps;

namespace {

std::string cert_path(const std::string& name) {
  return "build/certs/" + name;
}

bool file_exists(const std::string& path) {
  struct stat st {};

  return ::stat(path.c_str(), &st) == 0;
}

} // namespace

TEST(SslExtremeTest, SslContextInitSuccess) {
  ASSERT_TRUE(file_exists(cert_path("cert.pem"))) << "测试证书不存在";
  ASSERT_TRUE(file_exists(cert_path("key.pem"))) << "测试密钥不存在";

  SslConfig cfg;
  cfg.cert_file = cert_path("cert.pem");
  cfg.key_file = cert_path("key.pem");
  cfg.enabled = true;

  EXPECT_NO_THROW({ SslContext ctx(cfg); });
}

TEST(SslExtremeTest, SslContextInitInvalidCert) {
  SslConfig cfg;
  cfg.cert_file = "/tmp/nonexistent_cert.pem";
  cfg.key_file = cert_path("key.pem");
  cfg.enabled = true;

  EXPECT_THROW({ SslContext ctx(cfg); }, std::runtime_error);
}

TEST(SslExtremeTest, SslContextInitInvalidKey) {
  SslConfig cfg;
  cfg.cert_file = cert_path("cert.pem");
  cfg.key_file = "/tmp/nonexistent_key.pem";
  cfg.enabled = true;

  EXPECT_THROW({ SslContext ctx(cfg); }, std::runtime_error);
}

TEST(SslExtremeTest, CreateSslFromContext) {
  SslConfig cfg;
  cfg.cert_file = cert_path("cert.pem");
  cfg.key_file = cert_path("key.pem");
  cfg.enabled = true;

  SslContext ctx(cfg);
  ssl_st* ssl = ctx.create_ssl();
  ASSERT_NE(ssl, nullptr);
  ctx.shutdown_and_free(ssl);
}

TEST(SslExtremeTest, CreateSslMultipleTimes) {
  SslConfig cfg;
  cfg.cert_file = cert_path("cert.pem");
  cfg.key_file = cert_path("key.pem");
  cfg.enabled = true;

  SslContext ctx(cfg);

  for (int i = 0; i < 10; ++i) {
    ssl_st* ssl = ctx.create_ssl();
    ASSERT_NE(ssl, nullptr);
    ctx.shutdown_and_free(ssl);
  }
}

TEST(SslExtremeTest, CleanupAfterInit) {
  SslConfig cfg;
  cfg.cert_file = cert_path("cert.pem");
  cfg.key_file = cert_path("key.pem");
  cfg.enabled = true;

  ssl_st* ssl = nullptr;
  {
    SslContext ctx(cfg);
    ssl = ctx.create_ssl();
    ASSERT_NE(ssl, nullptr);
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
