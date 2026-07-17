#include "qps_runner.hpp"
#include "ssl_context.h"

int main() {
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

  return 0;
}
