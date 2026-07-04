#pragma once

#include <cstddef>
#include <string>

struct ssl_st;
struct ssl_ctx_st;

namespace hps {

struct SslConfig {
  std::string cert_file;
  std::string key_file;
  std::string ca_file;
  bool verify_peer{false};
  bool enabled{false};
};

class SslContext {
public:
  explicit SslContext(const SslConfig& config);
  ~SslContext();

  SslContext(const SslContext&) = delete;
  SslContext& operator=(const SslContext&) = delete;
  SslContext(SslContext&&) = delete;
  SslContext& operator=(SslContext&&) = delete;

  ssl_st* create_ssl();
  bool accept(ssl_st* ssl, int fd);
  void shutdown_and_free(ssl_st* ssl);

  static bool want_read(ssl_st* ssl, int ret);
  static bool want_write(ssl_st* ssl, int ret);
  static ssize_t read(ssl_st* ssl, void* buf, std::size_t len);
  static ssize_t write(ssl_st* ssl, const void* buf, std::size_t len);

private:
  ssl_ctx_st* ctx_;
  SslConfig config_;
};

} // namespace hps
