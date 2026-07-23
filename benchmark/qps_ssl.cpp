#include "qps_runner.hpp"
#include "ssl_context.h"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() noexcept {
  try {
    auto levels = hps::bench::default_qps_levels();

    // SslContext create (disabled mode)
    {
      hps::SslConfig config;
      config.enabled = false;
      config.cert_file.clear();
      config.key_file.clear();

      hps::bench::run_qps_steps("SslContext Create (disabled)", levels, [&config](int) {
        hps::SslContext ctx(config);
        (void)ctx;
      });
    }

    // SslContext create + accept + shutdown (disabled mode)
    {
      hps::SslConfig config;
      config.enabled = false;
      config.cert_file.clear();
      config.key_file.clear();
      hps::SslContext ctx(config);

      hps::bench::run_qps_steps("SslContext Create+Accept+Shutdown", levels, [&ctx](int) {
        auto* ssl = ctx.create_ssl();
        bool accepted = ctx.accept(ssl, -1);
        ctx.shutdown_and_free(ssl);
        (void)accepted;
      });
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "SSL QPS 基准失败: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "SSL QPS 基准失败: 未知异常\n";
  }
  return EXIT_FAILURE;
}
