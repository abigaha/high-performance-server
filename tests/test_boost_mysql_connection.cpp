#include "boost_mysql_connection.h"
#include "db_config.h"

#include <gtest/gtest.h>

#include <array>
#include <boost/asio.hpp>
#include <cstdint>
#include <exception>
#include <future>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace hps {
namespace {

namespace asio = boost::asio;
using Tcp = asio::ip::tcp;

constexpr std::uint32_t kClientSsl = 0x00000800U;
constexpr std::uint32_t kServerCapabilities = 0x01288a08U;

void append_uint16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_uint24(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
}

void append_uint32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
  append_uint24(bytes, value);
  bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void append_bytes(std::vector<std::uint8_t>& bytes, std::string_view value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> make_server_greeting() {
  std::vector<std::uint8_t> payload;
  payload.push_back(0x0aU);
  append_bytes(payload, "8.0.46");
  payload.push_back(0x00U);
  append_uint32(payload, 1U);
  append_bytes(payload, "abcdefgh");
  payload.push_back(0x00U);
  append_uint16(payload, static_cast<std::uint16_t>(kServerCapabilities & 0xffffU));
  payload.push_back(0x2dU);
  append_uint16(payload, 0x0002U);
  append_uint16(payload, static_cast<std::uint16_t>(kServerCapabilities >> 16U));
  payload.push_back(21U);
  payload.insert(payload.end(), 10U, 0x00U);
  append_bytes(payload, "ijklmnopqrst");
  payload.push_back(0x00U);
  append_bytes(payload, "caching_sha2_password");
  payload.push_back(0x00U);

  std::vector<std::uint8_t> packet;
  append_uint24(packet, static_cast<std::uint32_t>(payload.size()));
  packet.push_back(0x00U);
  packet.insert(packet.end(), payload.begin(), payload.end());
  return packet;
}

std::uint32_t read_client_capabilities(Tcp::socket& socket) {
  std::array<std::uint8_t, 4> header{};
  asio::read(socket, asio::buffer(header));

  const auto payload_size = static_cast<std::size_t>(header[0]) | (static_cast<std::size_t>(header[1]) << 8U) |
                            (static_cast<std::size_t>(header[2]) << 16U);
  std::vector<std::uint8_t> payload(payload_size);
  asio::read(socket, asio::buffer(payload));
  if (payload.size() < sizeof(std::uint32_t)) {
    throw std::runtime_error("客户端握手响应过短");
  }

  return static_cast<std::uint32_t>(payload[0]) | (static_cast<std::uint32_t>(payload[1]) << 8U) |
         (static_cast<std::uint32_t>(payload[2]) << 16U) | (static_cast<std::uint32_t>(payload[3]) << 24U);
}

} // namespace

TEST(BoostMySqlConnectionTest, RequestsTlsDuringHandshake) {
  asio::io_context server_context;
  Tcp::acceptor acceptor(server_context, Tcp::endpoint(Tcp::v4(), 0));
  const auto port = acceptor.local_endpoint().port();

  std::promise<std::uint32_t> captured_capabilities;
  auto capabilities_future = captured_capabilities.get_future();
  std::jthread server_thread([&]() {
    try {
      Tcp::socket socket(server_context);
      acceptor.accept(socket);
      const auto greeting = make_server_greeting();
      asio::write(socket, asio::buffer(greeting));
      captured_capabilities.set_value(read_client_capabilities(socket));
    } catch (...) {
      captured_capabilities.set_exception(std::current_exception());
    }
  });

  DbConfig config;
  config.host = "127.0.0.1";
  config.port = port;
  config.username = "tls-test";
  config.password = "tls-test-password";
  config.database = "tls-test-database";

  BoostMySqlConnection connection;
  EXPECT_FALSE(connection.connect(config));

  const auto capabilities = capabilities_future.get();
  EXPECT_NE(capabilities & kClientSsl, 0U);
  EXPECT_FALSE(connection.is_open());
}

} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
