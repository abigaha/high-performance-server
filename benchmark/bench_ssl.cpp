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

static void BM_SSL_ReadWriteThroughput(benchmark::State& state) {
  std::size_t size = static_cast<std::size_t>(state.range(0));
  std::string data(size, 'A');
  SslBenchFixture f;
  hps::SslContext ctx(f.config);
  for (auto _ : state) {
    auto* ssl = ctx.create_ssl();
    if (ssl != nullptr) {
      auto written = hps::SslContext::write(ssl, data.data(), data.size());
      benchmark::DoNotOptimize(written);
      ctx.shutdown_and_free(ssl);
    }
  }
  state.SetBytesProcessed(state.iterations() * size);
}

BENCHMARK(BM_SSL_ReadWriteThroughput)->Arg(1024)->Arg(65536)->Arg(262144);

static void BM_SSL_HandshakeLatency(benchmark::State& state) {
  SslBenchFixture f;
  for (auto _ : state) {
    hps::SslContext ctx(f.config);
    auto* ssl = ctx.create_ssl();
    bool ok = ctx.accept(ssl, -1);
    ctx.shutdown_and_free(ssl);
    benchmark::DoNotOptimize(ok);
  }
}

BENCHMARK(BM_SSL_HandshakeLatency);

BENCHMARK_MAIN();
