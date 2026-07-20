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

static void BM_WebSocket_ContinuousFrames(benchmark::State& state) {
  int num_frames = state.range(0);
  std::string payload(64, 'x');
  for (auto _ : state) {
    for (int i = 0; i < num_frames; ++i) {
      auto frame = hps::ws_encode_frame(hps::WsOpcode::TEXT, payload, false);
      auto decoded = hps::ws_decode_frame(std::string_view(frame.data(), frame.size()));
      benchmark::DoNotOptimize(decoded);
    }
  }
  state.SetItemsProcessed(state.iterations() * num_frames);
}

BENCHMARK(BM_WebSocket_ContinuousFrames)->Arg(100)->Arg(1000)->Arg(10000);

static void BM_WebSocket_LargePayload(benchmark::State& state) {
  std::size_t size = static_cast<std::size_t>(state.range(0));
  std::string payload(size, 'x');
  for (auto _ : state) {
    auto frame = hps::ws_encode_frame(hps::WsOpcode::BINARY, payload, false);
    auto decoded = hps::ws_decode_frame(std::string_view(frame.data(), frame.size()));
    benchmark::DoNotOptimize(decoded);
  }
  state.SetBytesProcessed(state.iterations() * size);
}

BENCHMARK(BM_WebSocket_LargePayload)->Arg(65536)->Arg(262144)->Arg(1048576);

BENCHMARK_MAIN();
