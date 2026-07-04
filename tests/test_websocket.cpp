#include "http_server.h"
#include "tcp_client.h"
#include "thread_pool.h"
#include "websocket.h"
#include "ws_connection.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace hps;

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace {

constexpr std::string_view kTestKey = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr std::string_view kExpectedAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

struct B64TestCase {
  std::string_view input;
  std::string_view expected;
};

constexpr B64TestCase kB64Cases[] = {
  {"", ""},
  {"f", "Zg=="},
  {"fo", "Zm8="},
  {"foo", "Zm9v"},
  {"foob", "Zm9vYg=="},
  {"fooba", "Zm9vYmE="},
  {"foobar", "Zm9vYmFy"},
};

HttpRequest make_ws_request(std::string_view key, std::string_view path = "/ws") {
  HttpRequest req;
  req.method = HttpMethod::GET;
  req.path = std::string(path);
  req.headers["Host"] = "localhost";
  req.headers["Upgrade"] = "websocket";
  req.headers["Connection"] = "Upgrade";
  req.headers["Sec-WebSocket-Key"] = std::string(key);
  req.headers["Sec-WebSocket-Version"] = "13";
  return req;
}

std::string send_raw(uint16_t port, const std::string& raw) {
  TcpClient client("127.0.0.1", port);
  if (!client.connect_to_server())
    return "";
  if (!client.send_message(raw))
    return "";
  std::string resp;
  client.receive_message(resp, ReadMode::RAW, 2000);
  return resp;
}

std::string make_masked_frame(WsOpcode opcode, std::string_view payload, uint32_t mask_key, bool fin = true) {
  std::string frame;
  frame.push_back(static_cast<char>((fin ? 0x80U : 0x00U) | static_cast<uint8_t>(opcode)));
  auto len = payload.size();
  frame.push_back(static_cast<char>(0x80U | (len < 126 ? len : 126U)));
  if (len >= 126) {
    auto ulen = static_cast<uint32_t>(len);
    frame.push_back(static_cast<char>((ulen >> 8U) & 0xFFU));
    frame.push_back(static_cast<char>(ulen & 0xFFU));
  }
  frame.push_back(static_cast<char>((mask_key >> 24U) & 0xFFU));
  frame.push_back(static_cast<char>((mask_key >> 16U) & 0xFFU));
  frame.push_back(static_cast<char>((mask_key >> 8U) & 0xFFU));
  frame.push_back(static_cast<char>(mask_key & 0xFFU));
  auto mk0 = static_cast<uint8_t>((mask_key >> 24U) & 0xFFU);
  auto mk1 = static_cast<uint8_t>((mask_key >> 16U) & 0xFFU);
  auto mk2 = static_cast<uint8_t>((mask_key >> 8U) & 0xFFU);
  auto mk3 = static_cast<uint8_t>(mask_key & 0xFFU);
  const std::array<uint8_t, 4> mk_arr = {mk0, mk1, mk2, mk3};
  for (size_t i = 0; i < len; ++i) {
    frame.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mk_arr[i % 4]));
  }
  return frame;
}

} // namespace

// T1: 握手正确性
TEST(WsHandshakeTest, ValidKey) {
  HttpRequest req = make_ws_request(kTestKey);
  HttpResponse resp;
  ASSERT_TRUE(ws_server_handshake(req, resp));
  EXPECT_EQ(resp.status_code, 101);
  EXPECT_EQ(resp.status_text, "Switching Protocols");
  auto it = resp.headers.find("Upgrade");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "websocket");
  auto ait = resp.headers.find("Sec-WebSocket-Accept");
  ASSERT_NE(ait, resp.headers.end());
  EXPECT_EQ(ait->second, kExpectedAccept);
}

// T2: 缺少 Sec-WebSocket-Key 应返回 false
TEST(WsHandshakeTest, MissingKey) {
  HttpRequest req;
  req.headers["Upgrade"] = "websocket";
  req.headers["Connection"] = "Upgrade";
  HttpResponse resp;
  EXPECT_FALSE(ws_server_handshake(req, resp));
}

// T3: 缺少 Upgrade 头应返回 false
TEST(WsHandshakeTest, MissingUpgrade) {
  HttpRequest req;
  req.headers["Sec-WebSocket-Key"] = "dGVzdA==";
  HttpResponse resp;
  EXPECT_FALSE(ws_server_handshake(req, resp));
}

// T4: TEXT 帧编码
TEST(WsFrameTest, EncodeText) {
  std::string payload = "Hello";
  auto frame = ws_encode_frame(WsOpcode::TEXT, payload);
  ASSERT_GE(frame.size(), 2);
  auto first = static_cast<uint8_t>(frame[0]);
  EXPECT_TRUE(first & 0x80); // FIN
  EXPECT_EQ(static_cast<uint8_t>(first & 0x0F), static_cast<uint8_t>(WsOpcode::TEXT));
  auto second = static_cast<uint8_t>(frame[1]);
  EXPECT_FALSE(second & 0x80); // 服务端不 mask
  EXPECT_EQ(static_cast<size_t>(second & 0x7F), payload.size());
  // payload 部分
  std::string decoded(frame.begin() + 2, frame.end());
  EXPECT_EQ(decoded, payload);
}

// T5: BINARY 帧编码（含 0x00）
TEST(WsFrameTest, EncodeBinary) {
  std::vector<char> payload = {static_cast<char>(0x00),
                               static_cast<char>(0x01),
                               static_cast<char>(0xFF),
                               static_cast<char>(0xAB)};
  auto frame = ws_encode_frame(WsOpcode::BINARY, payload);
  ASSERT_GE(frame.size(), 2);
  auto first = static_cast<uint8_t>(frame[0]);
  EXPECT_TRUE(first & 0x80);
  EXPECT_EQ(static_cast<uint8_t>(first & 0x0F), static_cast<uint8_t>(WsOpcode::BINARY));
  std::vector<char> decoded(frame.begin() + 2, frame.end());
  ASSERT_EQ(decoded.size(), payload.size());
  for (size_t i = 0; i < payload.size(); ++i) {
    EXPECT_EQ(decoded[i], payload[i]);
  }
}

// T6: CLOSE 帧编码（带关闭码）
TEST(WsFrameTest, EncodeClose) {
  std::vector<char> close_payload(2);
  close_payload[0] = static_cast<char>((1000 >> 8) & 0xFF);
  close_payload[1] = static_cast<char>(1000 & 0xFF);
  auto frame = ws_encode_frame(WsOpcode::CLOSE, close_payload);
  auto decoded_opt = ws_decode_frame(std::string_view(frame.data(), frame.size()));
  ASSERT_TRUE(decoded_opt.has_value());
  EXPECT_EQ(decoded_opt->opcode, WsOpcode::CLOSE);
  EXPECT_TRUE(decoded_opt->fin);
  auto code = ws_close_code(*decoded_opt);
  ASSERT_TRUE(code.has_value());
  EXPECT_EQ(*code, 1000);
}

// T7: 掩码帧解码正确
TEST(WsFrameTest, DecodeMasked) {
  std::string raw = make_masked_frame(WsOpcode::TEXT, "test", 0x12345678);
  auto frame_opt = ws_decode_frame(raw);
  ASSERT_TRUE(frame_opt.has_value());
  EXPECT_EQ(frame_opt->opcode, WsOpcode::TEXT);
  EXPECT_TRUE(frame_opt->fin);
  std::string payload_str(frame_opt->payload.begin(), frame_opt->payload.end());
  EXPECT_EQ(payload_str, "test");
}

// T8: 未 mask 帧解码（服务端帧）
TEST(WsFrameTest, DecodeUnmasked) {
  auto encoded = ws_encode_frame(WsOpcode::TEXT, std::string_view("hello"));
  auto frame_opt = ws_decode_frame(std::string_view(encoded.data(), encoded.size()));
  ASSERT_TRUE(frame_opt.has_value());
  std::string payload_str(frame_opt->payload.begin(), frame_opt->payload.end());
  EXPECT_EQ(payload_str, "hello");
}

// T9: 不完整帧返回 nullopt
TEST(WsFrameTest, DecodeIncomplete) {
  std::string incomplete = "\x81";
  auto frame_opt = ws_decode_frame(incomplete);
  EXPECT_FALSE(frame_opt.has_value());
}

// T10: 分片帧解码
TEST(WsFrameTest, DecodeFragmented) {
  // 构造两个分片帧
  std::string frame1;
  frame1.push_back(static_cast<char>(0x01)); // FIN=0, TEXT
  frame1.push_back(static_cast<char>(3));
  frame1 += "Hel";

  std::string frame2;
  frame2.push_back(static_cast<char>(0x80)); // FIN=1, CONTINUATION
  frame2.push_back(static_cast<char>(2));
  frame2 += "lo";

  // 分开解码
  auto f1 = ws_decode_frame(frame1);
  ASSERT_TRUE(f1.has_value());
  EXPECT_FALSE(f1->fin);
  EXPECT_EQ(f1->opcode, WsOpcode::TEXT);

  auto f2 = ws_decode_frame(frame2);
  ASSERT_TRUE(f2.has_value());
  EXPECT_TRUE(f2->fin);
  EXPECT_EQ(f2->opcode, WsOpcode::CONTINUATION);

  // 手动组合
  std::string combined(f1->payload.begin(), f1->payload.end());
  combined.append(f2->payload.begin(), f2->payload.end());
  EXPECT_EQ(combined, "Hello");
}

// T11: Base64 编码
TEST(WsBase64Test, Encode) {
  for (const auto& tc : kB64Cases) {
    auto input_bytes = std::vector<uint8_t>(tc.input.begin(), tc.input.end());
    auto result = base64_encode(input_bytes);
    EXPECT_EQ(result, tc.expected) << "input: '" << tc.input << "'";
  }
}

// T12: 端到端 WebSocket 集成（HttpServer + 握手）
TEST(WsIntegrationTest, ServerHandshake) {
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.ws("/chat", [](const HttpRequest& req, std::shared_ptr<WsConnection>) {
    // 用户自定义 WS handler
    (void)req;
  });

  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  // 发送 WebSocket 升级请求
  std::string upgrade_req = "GET /chat HTTP/1.1\r\n"
                            "Host: localhost\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                            "Sec-WebSocket-Version: 13\r\n"
                            "\r\n";

  auto resp = send_raw(port, upgrade_req);
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp.find("101 Switching Protocols"), std::string::npos) << "响应: " << resp;
  ASSERT_NE(resp.find("Upgrade: websocket"), std::string::npos) << "响应: " << resp;
  ASSERT_NE(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos) << "响应: " << resp;
}
