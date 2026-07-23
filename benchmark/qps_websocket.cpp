#include "qps_runner.hpp"
#include "websocket.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

int main() noexcept {
  try {
    auto levels = hps::bench::default_qps_levels();

    // Encode text frames (256 bytes)
    {
      std::string payload(256, 'x');
      hps::bench::run_qps_steps("WebSocket Encode Text 256B", levels, [&payload](int) {
        auto frame = hps::ws_encode_frame(hps::WsOpcode::TEXT, payload, false);
        (void)frame;
      });
    }

    // Decode frames (256 bytes)
    {
      std::string payload(256, 'x');
      auto frame = hps::ws_encode_frame(hps::WsOpcode::TEXT, payload, false);
      std::string_view frame_sv(frame.data(), frame.size());
      hps::bench::run_qps_steps("WebSocket Decode 256B", levels, [&frame_sv](int) {
        auto decoded = hps::ws_decode_frame(frame_sv);
        (void)decoded;
      });
    }

    // Encode binary frames with mask (1KB)
    {
      std::string payload(1024, '\xff');
      hps::bench::run_qps_steps("WebSocket Encode Binary Masked 1KB", levels, [&payload](int) {
        auto frame = hps::ws_encode_frame(hps::WsOpcode::BINARY, payload, true);
        (void)frame;
      });
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "WebSocket QPS 基准失败: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "WebSocket QPS 基准失败: 未知异常\n";
  }
  return EXIT_FAILURE;
}
