#include "websocket.h"

#include <openssl/sha.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hps {

namespace {

constexpr std::string_view kMagicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr std::string_view kBase64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::string base64_encode(std::span<const uint8_t> input) {
  if (input.empty()) {
    return {};
  }

  auto len = input.size();
  auto out_len = ((len + 2) / 3) * 4;
  std::string result(out_len, '=');

  std::size_t i = 0;
  std::size_t j = 0;
  while (i < len) {
    auto remaining = len - i;
    auto octet_a = static_cast<uint32_t>(input[i++]);
    auto octet_b = (remaining > 1) ? static_cast<uint32_t>(input[i++]) : 0U;
    auto octet_c = (remaining > 2) ? static_cast<uint32_t>(input[i++]) : 0U;

    auto triple = (octet_a << 16U) | (octet_b << 8U) | octet_c;

    result[j++] = kBase64Chars[(triple >> 18U) & 0x3FU];
    result[j++] = kBase64Chars[(triple >> 12U) & 0x3FU];
    result[j++] = (remaining > 1) ? kBase64Chars[(triple >> 6U) & 0x3FU] : '=';
    result[j++] = (remaining > 2) ? kBase64Chars[triple & 0x3FU] : '=';
  }

  return result;
}

bool ws_server_handshake(const HttpRequest& req, HttpResponse& resp) {
  auto up_it = req.headers.find("Upgrade");
  if (up_it == req.headers.end()) {
    return false;
  }
  auto conn_it = req.headers.find("Connection");
  if (conn_it == req.headers.end()) {
    return false;
  }
  auto key_it = req.headers.find("Sec-WebSocket-Key");
  if (key_it == req.headers.end()) {
    return false;
  }

  std::string concat = key_it->second + std::string(kMagicGuid);

  std::array<unsigned char, SHA_DIGEST_LENGTH> sha_buf{};
  SHA1(reinterpret_cast<const unsigned char*>(concat.data()), concat.size(), sha_buf.data());

  std::string accept = base64_encode(std::span<const uint8_t>(sha_buf.data(), sha_buf.size()));

  resp.set_status(101, "Switching Protocols");
  resp.set_header("Upgrade", "websocket");
  resp.set_header("Connection", "Upgrade");
  resp.set_header("Sec-WebSocket-Accept", accept);
  return true;
}

std::vector<char> ws_encode_frame(WsOpcode opcode, std::span<const char> payload, bool fin) {
  std::vector<char> frame;
  auto len = payload.size();

  auto first_byte = static_cast<uint8_t>((fin ? 0x80U : 0x00U) | static_cast<uint8_t>(opcode));
  frame.push_back(static_cast<char>(first_byte));

  if (len < 126) {
    frame.push_back(static_cast<char>(len));
  } else if (len <= 0xFFFFU) {
    frame.push_back(static_cast<char>(126));
    frame.push_back(static_cast<char>((len >> 8U) & 0xFFU));
    frame.push_back(static_cast<char>(len & 0xFFU));
  } else {
    frame.push_back(static_cast<char>(127));
    for (auto i = 7; i >= 0; --i) {
      frame.push_back(static_cast<char>((len >> (static_cast<std::size_t>(i) * 8U)) & 0xFFU));
    }
  }

  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

std::optional<WsFrame> ws_decode_frame(std::string_view data) {
  if (data.size() < 2) {
    return std::nullopt;
  }

  auto first = static_cast<uint8_t>(data[0]);
  auto second = static_cast<uint8_t>(data[1]);

  bool fin = (first & 0x80U) != 0U;
  auto opcode = static_cast<WsOpcode>(first & 0x0FU);
  bool masked = (second & 0x80U) != 0U;

  std::size_t offset = 2;
  std::uint64_t payload_len = second & 0x7FU;

  if (payload_len == 126) {
    if (data.size() < 4) {
      return std::nullopt;
    }
    payload_len = (static_cast<uint64_t>(static_cast<uint8_t>(data[2])) << 8U) |
                  static_cast<uint64_t>(static_cast<uint8_t>(data[3]));
    offset = 4;
  } else if (payload_len == 127) {
    if (data.size() < 10) {
      return std::nullopt;
    }
    payload_len = 0;
    for (auto i = 0; i < 8; ++i) {
      payload_len =
        (payload_len << 8U) |
        static_cast<uint64_t>(static_cast<uint8_t>(data[static_cast<std::size_t>(2) + static_cast<std::size_t>(i)]));
    }
    offset = 10;
  }

  std::array<uint8_t, 4> mask_key{};
  if (masked) {
    if (data.size() < offset + 4) {
      return std::nullopt;
    }
    mask_key[0] = static_cast<uint8_t>(data[offset]);
    mask_key[1] = static_cast<uint8_t>(data[offset + 1]);
    mask_key[2] = static_cast<uint8_t>(data[offset + 2]);
    mask_key[3] = static_cast<uint8_t>(data[offset + 3]);
    offset += 4;
  }

  if (data.size() < offset + payload_len) {
    return std::nullopt;
  }

  std::vector<char> payload(static_cast<std::size_t>(payload_len));
  for (std::size_t i = 0; i < static_cast<std::size_t>(payload_len); ++i) {
    auto c = data[offset + i];
    if (masked) {
      c = static_cast<char>(static_cast<uint8_t>(c) ^ mask_key[i % 4]);
    }
    payload[i] = c;
  }

  return WsFrame{fin, opcode, std::move(payload)};
}

std::optional<uint16_t> ws_close_code(const WsFrame& frame) {
  if (frame.payload.size() < 2) {
    return std::nullopt;
  }
  auto hi = static_cast<uint16_t>(static_cast<uint8_t>(frame.payload[0]));
  auto lo = static_cast<uint16_t>(static_cast<uint8_t>(frame.payload[1]));
  return static_cast<uint16_t>((static_cast<uint32_t>(hi) << 8U) | lo);
}

} // namespace hps
