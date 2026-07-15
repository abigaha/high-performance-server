#include "ssl_context.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <string>
#include <vector>

namespace {

struct SslBenchFixture {
  hps::SslConfig config;

  SslBenchFixture() {
    config.enabled = false;
    config.cert_file.clear();
    config.key_file.clear();
  }
};

} // namespace

static void BM_SslContext_Create(benchmark::State& state) {
  SslBenchFixture f;
  for (auto _ : state) {
    hps::SslContext ctx(f.config);
    benchmark::DoNotOptimize(ctx);
  }
}

BENCHMARK(BM_SslContext_Create);

static void BM_SslContext_NotEnabled(benchmark::State& state) {
  SslBenchFixture f;
  for (auto _ : state) {
    hps::SslContext ctx(f.config);
    auto* ssl = ctx.create_ssl();
    bool accepted = ctx.accept(ssl, -1);
    ctx.shutdown_and_free(ssl);
    benchmark::DoNotOptimize(accepted);
  }
}

BENCHMARK(BM_SslContext_NotEnabled);

BENCHMARK_MAIN();
