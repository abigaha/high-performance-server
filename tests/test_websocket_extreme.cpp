#include "websocket.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace hps;

TEST(WebSocketExtremeTest, FrameVeryLargePayload) {
  std::vector<char> payload(1024 * 1024, 'B');
  auto frame = ws_encode_frame(WsOpcode::TEXT, payload);
  ASSERT_GT(frame.size(), payload.size());
  auto decoded = ws_decode_frame(std::string_view(frame.data(), frame.size()));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->opcode, WsOpcode::TEXT);
  EXPECT_TRUE(decoded->fin);
  EXPECT_EQ(decoded->payload.size(), payload.size());
  EXPECT_EQ(std::memcmp(decoded->payload.data(), payload.data(), payload.size()), 0);
}

TEST(WebSocketExtremeTest, FrameMaxPayload16Bit) {
  std::vector<char> payload(65535, 'M');
  auto frame = ws_encode_frame(WsOpcode::BINARY, payload);
  ASSERT_GT(frame.size(), payload.size());
  auto decoded = ws_decode_frame(std::string_view(frame.data(), frame.size()));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->opcode, WsOpcode::BINARY);
  EXPECT_EQ(decoded->payload.size(), payload.size());
}

TEST(WebSocketExtremeTest, DecodeFragmentedFrame) {
  // 第一个分片 FIN=0, TEXT
  std::string frag1;
  frag1.push_back(static_cast<char>(0x01));
  frag1.push_back(static_cast<char>(3));
  frag1 += "Hel";

  // 第二个分片 FIN=1, CONTINUATION
  std::string frag2;
  frag2.push_back(static_cast<char>(0x80));
  frag2.push_back(static_cast<char>(2));
  frag2 += "lo";

  auto f1 = ws_decode_frame(frag1);
  ASSERT_TRUE(f1.has_value());
  EXPECT_FALSE(f1->fin);
  EXPECT_EQ(f1->opcode, WsOpcode::TEXT);

  auto f2 = ws_decode_frame(frag2);
  ASSERT_TRUE(f2.has_value());
  EXPECT_TRUE(f2->fin);
  EXPECT_EQ(f2->opcode, WsOpcode::CONTINUATION);

  std::string combined(f1->payload.begin(), f1->payload.end());
  combined.append(f2->payload.begin(), f2->payload.end());
  EXPECT_EQ(combined, "Hello");
}

TEST(WebSocketExtremeTest, EncodeAllOpcodeTypes) {
  std::vector<char> payload = {'d', 'a', 't', 'a'};
  std::vector<WsOpcode> opcodes = {WsOpcode::TEXT, WsOpcode::BINARY, WsOpcode::CLOSE, WsOpcode::PING, WsOpcode::PONG};

  for (auto op : opcodes) {
    auto frame = ws_encode_frame(op, payload);
    ASSERT_GE(frame.size(), 2);
    auto first = static_cast<uint8_t>(frame[0]);
    EXPECT_TRUE(first & 0x80) << "FIN should be set for opcode " << static_cast<int>(op);
    EXPECT_EQ(static_cast<uint8_t>(first & 0x0F), static_cast<uint8_t>(op))
      << "Opcode mismatch for " << static_cast<int>(op);

    auto decoded = ws_decode_frame(std::string_view(frame.data(), frame.size()));
    ASSERT_TRUE(decoded.has_value()) << "Decode failed for opcode " << static_cast<int>(op);
    EXPECT_EQ(decoded->opcode, op);
  }
}

TEST(WebSocketExtremeTest, PingPongRoundtrip) {
  std::vector<char> payload = {'p', 'i', 'n', 'g'};

  auto ping_frame = ws_encode_frame(WsOpcode::PING, payload);
  auto decoded = ws_decode_frame(std::string_view(ping_frame.data(), ping_frame.size()));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->opcode, WsOpcode::PING);

  // 从 PING payload 构造 PONG 帧
  auto pong_frame = ws_encode_frame(WsOpcode::PONG, decoded->payload);
  auto decoded_pong = ws_decode_frame(std::string_view(pong_frame.data(), pong_frame.size()));
  ASSERT_TRUE(decoded_pong.has_value());
  EXPECT_EQ(decoded_pong->opcode, WsOpcode::PONG);
  std::string pong_payload(decoded_pong->payload.begin(), decoded_pong->payload.end());
  EXPECT_EQ(pong_payload, "ping");
}

TEST(WebSocketExtremeTest, CloseFrameWithCode) {
  std::vector<char> close_payload(2);
  close_payload[0] = static_cast<char>((1000 >> 8) & 0xFF);
  close_payload[1] = static_cast<char>(1000 & 0xFF);

  auto frame = ws_encode_frame(WsOpcode::CLOSE, close_payload);
  auto decoded = ws_decode_frame(std::string_view(frame.data(), frame.size()));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->opcode, WsOpcode::CLOSE);

  auto code = ws_close_code(*decoded);
  ASSERT_TRUE(code.has_value());
  EXPECT_EQ(*code, 1000);
}

TEST(WebSocketExtremeTest, MaskedFrameDecode) {
  // 手动构造带 mask 的帧：TEXT, payload="masked"
  std::string payload_str = "masked";
  uint32_t mask_key = 0xDEADBEEF;

  std::string frame;
  frame.push_back(static_cast<char>(0x81));                      // FIN=1, TEXT
  frame.push_back(static_cast<char>(0x80 | payload_str.size())); // MASK=1, len=6
  frame.push_back(static_cast<char>((mask_key >> 24) & 0xFF));
  frame.push_back(static_cast<char>((mask_key >> 16) & 0xFF));
  frame.push_back(static_cast<char>((mask_key >> 8) & 0xFF));
  frame.push_back(static_cast<char>(mask_key & 0xFF));
  auto mk_arr = std::array<uint8_t, 4>{
    static_cast<uint8_t>((mask_key >> 24) & 0xFF),
    static_cast<uint8_t>((mask_key >> 16) & 0xFF),
    static_cast<uint8_t>((mask_key >> 8) & 0xFF),
    static_cast<uint8_t>(mask_key & 0xFF),
  };
  for (size_t i = 0; i < payload_str.size(); ++i) {
    frame.push_back(static_cast<char>(static_cast<uint8_t>(payload_str[i]) ^ mk_arr[i % 4]));
  }

  auto decoded = ws_decode_frame(frame);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->opcode, WsOpcode::TEXT);
  std::string decoded_payload(decoded->payload.begin(), decoded->payload.end());
  EXPECT_EQ(decoded_payload, "masked");
}

TEST(WebSocketExtremeTest, EmptyPayloadFrame) {
  auto frame = ws_encode_frame(WsOpcode::TEXT, std::vector<char>{});
  ASSERT_GE(frame.size(), 2);
  auto first = static_cast<uint8_t>(frame[0]);
  EXPECT_TRUE(first & 0x80);
  EXPECT_EQ(static_cast<uint8_t>(first & 0x0F), static_cast<uint8_t>(WsOpcode::TEXT));

  auto decoded = ws_decode_frame(std::string_view(frame.data(), frame.size()));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->payload.empty());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
