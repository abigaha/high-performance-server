#pragma once

#include "http_request.h"
#include "http_response.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hps {

enum class WsOpcode : uint8_t {
  CONTINUATION = 0x0,
  TEXT = 0x1,
  BINARY = 0x2,
  CLOSE = 0x8,
  PING = 0x9,
  PONG = 0xA,
};

struct WsFrame {
  bool fin;
  WsOpcode opcode;
  std::vector<char> payload;
};

std::string base64_encode(std::span<const uint8_t> input);

bool ws_server_handshake(const HttpRequest& req, HttpResponse& resp);

std::vector<char> ws_encode_frame(WsOpcode opcode, std::span<const char> payload, bool fin = true);

std::optional<WsFrame> ws_decode_frame(std::string_view data);

std::optional<uint16_t> ws_close_code(const WsFrame& frame);

} // namespace hps
