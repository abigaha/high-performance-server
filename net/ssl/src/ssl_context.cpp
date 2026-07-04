#include "ssl_context.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <stdexcept>
#include <string>

namespace hps {

namespace {

constexpr auto kSslOptions = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1;

void ensure_openssl() {
  static struct Init {
    Init() {
      SSL_library_init();
      SSL_load_error_strings();
      OpenSSL_add_all_algorithms();
    }
  } init;

  (void)init;
}

} // anonymous namespace

SslContext::SslContext(const SslConfig& config) :
    config_(config), ctx_([] {
      ensure_openssl();
      auto* c = SSL_CTX_new(TLS_server_method());
      if (c == nullptr) {
        throw std::runtime_error("SSL_CTX_new 失败");
      }
      return c;
    }()) {
  SSL_CTX_set_options(ctx_, kSslOptions);

  if (!config_.cert_file.empty()) {
    if (SSL_CTX_use_certificate_file(ctx_, config_.cert_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
      SSL_CTX_free(ctx_);
      ctx_ = nullptr;
      throw std::runtime_error("加载证书失败: " + config_.cert_file);
    }
  }

  if (!config_.key_file.empty()) {
    if (SSL_CTX_use_PrivateKey_file(ctx_, config_.key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
      SSL_CTX_free(ctx_);
      ctx_ = nullptr;
      throw std::runtime_error("加载私钥失败: " + config_.key_file);
    }

    if (SSL_CTX_check_private_key(ctx_) <= 0) {
      SSL_CTX_free(ctx_);
      ctx_ = nullptr;
      throw std::runtime_error("私钥与证书不匹配");
    }
  }

  if (!config_.ca_file.empty()) {
    if (SSL_CTX_load_verify_locations(ctx_, config_.ca_file.c_str(), nullptr) <= 0) {
      SSL_CTX_free(ctx_);
      ctx_ = nullptr;
      throw std::runtime_error("加载 CA 证书失败: " + config_.ca_file);
    }
  }

  if (config_.verify_peer) {
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
  } else {
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
  }
}

SslContext::~SslContext() {
  if (ctx_ != nullptr) {
    SSL_CTX_free(ctx_);
  }
}

ssl_st* SslContext::create_ssl() {
  if (ctx_ == nullptr) {
    return nullptr;
  }
  ssl_st* ssl = SSL_new(ctx_);
  if (ssl == nullptr) {
    throw std::runtime_error("SSL_new 失败");
  }
  SSL_set_accept_state(ssl);
  return ssl;
}

bool SslContext::accept(ssl_st* ssl, int fd) {
  if (ssl == nullptr) {
    return false;
  }
  SSL_set_fd(ssl, fd);
  int ret = SSL_accept(static_cast<SSL*>(ssl));
  return ret == 1;
}

void SslContext::shutdown_and_free(ssl_st* ssl) {
  if (ssl == nullptr) {
    return;
  }
  SSL_shutdown(static_cast<SSL*>(ssl));
  SSL_free(static_cast<SSL*>(ssl));
}

bool SslContext::want_read(ssl_st* ssl, int ret) {
  return SSL_get_error(static_cast<SSL*>(ssl), ret) == SSL_ERROR_WANT_READ;
}

bool SslContext::want_write(ssl_st* ssl, int ret) {
  return SSL_get_error(static_cast<SSL*>(ssl), ret) == SSL_ERROR_WANT_WRITE;
}

ssize_t SslContext::read(ssl_st* ssl, void* buf, std::size_t len) {
  if (ssl == nullptr) {
    return -1;
  }
  ssize_t n = SSL_read(static_cast<SSL*>(ssl), buf, static_cast<int>(len));
  if (n > 0) {
    return n;
  }
  int err = SSL_get_error(static_cast<SSL*>(ssl), static_cast<int>(n));
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    return 0;
  }
  if (err == SSL_ERROR_ZERO_RETURN) {
    return 0;
  }
  return -1;
}

ssize_t SslContext::write(ssl_st* ssl, const void* buf, std::size_t len) {
  if (ssl == nullptr) {
    return -1;
  }
  ssize_t n = SSL_write(static_cast<SSL*>(ssl), buf, static_cast<int>(len));
  if (n > 0) {
    return n;
  }
  int err = SSL_get_error(static_cast<SSL*>(ssl), static_cast<int>(n));
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    return 0;
  }
  if (err == SSL_ERROR_ZERO_RETURN) {
    return 0;
  }
  return -1;
}

} // namespace hps
