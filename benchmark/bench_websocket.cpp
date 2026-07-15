#include "websocket.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

static void BM_WebSocket_EncodeText(benchmark::State& state) {
  std::string payload(static_cast<std::size_t>(state.range(0)), 'x');
  for (auto _ : state) {
    auto frame = hps::ws_encode_frame(hps::WsOpcode::TEXT, payload, false);
    benchmark::DoNotOptimize(frame);
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_WebSocket_EncodeText)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(65536);

static void BM_WebSocket_DecodeFrame(benchmark::State& state) {
  std::string payload(static_cast<std::size_t>(state.range(0)), 'x');
  auto frame = hps::ws_encode_frame(hps::WsOpcode::TEXT, payload, false);
  std::string_view frame_sv(frame.data(), frame.size());
  for (auto _ : state) {
    auto decoded = hps::ws_decode_frame(frame_sv);
    benchmark::DoNotOptimize(decoded);
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_WebSocket_DecodeFrame)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(65536);

static void BM_WebSocket_EncodeBinary(benchmark::State& state) {
  std::string payload(static_cast<std::size_t>(state.range(0)), '\xff');
  for (auto _ : state) {
    auto frame = hps::ws_encode_frame(hps::WsOpcode::BINARY, payload, true);
    benchmark::DoNotOptimize(frame);
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_WebSocket_EncodeBinary)->Arg(1024);

static void BM_WebSocket_DecodeWithMask(benchmark::State& state) {
  std::string payload(static_cast<std::size_t>(state.range(0)), 'x');
  auto frame = hps::ws_encode_frame(hps::WsOpcode::TEXT, payload, true);
  std::string_view frame_sv(frame.data(), frame.size());
  for (auto _ : state) {
    auto decoded = hps::ws_decode_frame(frame_sv);
    benchmark::DoNotOptimize(decoded);
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_WebSocket_DecodeWithMask)->Arg(1024);

BENCHMARK_MAIN();
