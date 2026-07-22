#include "auth_service.h"
#include "http_server.h"
#include "tcp_client.h"
#include "upload_policy.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>

using namespace hps;

namespace {

class UploadAuthService : public IAuthService {
public:
  AuthUser validate_token(const std::string& token) override {
    if (token == "normal-token") {
      return {1, "normal", UserRole::NORMAL};
    }
    if (token == "vip-token") {
      return {2, "vip", UserRole::VIP};
    }
    return {};
  }

  std::string generate_token(const AuthUser& user) override {
    static_cast<void>(user);
    return "token";
  }

  std::optional<AuthUser> authenticate(const std::string& username, const std::string& password) override {
    static_cast<void>(username);
    static_cast<void>(password);
    return std::nullopt;
  }
};

std::string send_raw(uint16_t port, const std::string& request) {
  TcpClient client("127.0.0.1", port);
  if (!client.connect_to_server() || !client.send_message(request)) {
    return {};
  }
  std::string response;
  client.receive_message(response, ReadMode::RAW, 3000);
  return response;
}

std::optional<HttpResponse> run_preflight(const ServerConfig& config,
                                          const HttpRequest& request,
                                          const UploadStreamContext& context) {
  const auto validation =
    validate_audio_upload(context.file_name, context.content_length, request.auth_user.role, config);
  if (validation.accepted) {
    return std::nullopt;
  }
  return make_upload_validation_response(validation);
}

} // namespace

TEST(UploadPolicyTest, RecognizesNineAudioExtensionsCaseInsensitively) {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 9> cases{{
    {"track.MP3", "audio/mpeg"},
    {"track.OgG", "audio/ogg"},
    {"track.WAV", "audio/wav"},
    {"track.FlAc", "audio/flac"},
    {"track.AAC", "audio/aac"},
    {"track.M4A", "audio/mp4"},
    {"track.WmA", "audio/x-ms-wma"},
    {"track.APE", "audio/x-monkeys-audio"},
    {"track.OpUs", "audio/opus"},
  }};

  for (const auto& [file_name, expected_type] : cases) {
    const auto actual_type = audio_content_type(file_name);
    ASSERT_TRUE(actual_type.has_value()) << file_name;
    EXPECT_EQ(*actual_type, expected_type) << file_name;
  }
}

TEST(UploadPolicyTest, RejectsInvalidNameTypeAndEmptyContent) {
  const ServerConfig config;

  const auto missing_name = validate_audio_upload("", 1, UserRole::NORMAL, config);
  EXPECT_FALSE(missing_name.accepted);
  EXPECT_EQ(missing_name.status_code, 400);
  EXPECT_EQ(missing_name.code, "INVALID_FILE_NAME");

  for (const std::string_view file_name : {"README", "track", "track.txt", "track.mp3.exe"}) {
    const auto unsupported = validate_audio_upload(file_name, 1, UserRole::NORMAL, config);
    EXPECT_FALSE(unsupported.accepted) << file_name;
    EXPECT_EQ(unsupported.status_code, 415) << file_name;
    EXPECT_EQ(unsupported.code, "UNSUPPORTED_FILE_TYPE") << file_name;
  }

  const auto missing_length = validate_audio_upload("track.mp3", std::nullopt, UserRole::NORMAL, config);
  EXPECT_EQ(missing_length.status_code, 400);
  EXPECT_EQ(missing_length.code, "INVALID_CONTENT_LENGTH");

  const auto empty = validate_audio_upload("track.mp3", 0, UserRole::NORMAL, config);
  EXPECT_EQ(empty.status_code, 400);
  EXPECT_EQ(empty.code, "EMPTY_FILE");
}

TEST(UploadPolicyTest, AppliesConfiguredNormalAndVipLimitsInclusively) {
  ServerConfig config;
  config.normal_max_size = 10 * 1024 * 1024;
  config.vip_max_size = 100 * 1024 * 1024;

  EXPECT_TRUE(validate_audio_upload("track.mp3", config.normal_max_size, UserRole::NORMAL, config).accepted);
  const auto normal_too_large =
    validate_audio_upload("track.mp3", static_cast<std::size_t>(config.normal_max_size) + 1, UserRole::NORMAL, config);
  EXPECT_EQ(normal_too_large.status_code, 413);
  EXPECT_EQ(normal_too_large.code, "FILE_TOO_LARGE");
  EXPECT_EQ(normal_too_large.max_size, static_cast<std::size_t>(config.normal_max_size));

  EXPECT_TRUE(validate_audio_upload("track.flac", config.vip_max_size, UserRole::VIP, config).accepted);
  const auto vip_too_large =
    validate_audio_upload("track.flac", static_cast<std::size_t>(config.vip_max_size) + 1, UserRole::VIP, config);
  EXPECT_EQ(vip_too_large.status_code, 413);
  EXPECT_EQ(vip_too_large.max_size, static_cast<std::size_t>(config.vip_max_size));
}

TEST(UploadPolicyTest, BuildsStructuredJsonErrorWithStableCodeAndDetails) {
  const ServerConfig config;
  const auto validation = validate_audio_upload("payload.bin", 5, UserRole::NORMAL, config);
  const auto response = make_upload_validation_response(validation);

  EXPECT_EQ(response.status_code, 415);
  EXPECT_EQ(response.headers.at("Content-Type"), "application/json; charset=utf-8");
  const auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(body.at("code"), "UNSUPPORTED_FILE_TYPE");
  EXPECT_TRUE(body.at("error").is_string());
  ASSERT_TRUE(body.at("details").at("allowed_extensions").is_array());
  EXPECT_EQ(body.at("details").at("allowed_extensions").size(), 9U);
}

TEST(UploadPolicyHttpTest, RejectsUnsupportedTypeBeforeSetupStorageAndHandler) {
  ServerConfig config;
  UploadAuthService auth;
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(auth);

  std::atomic<int> setup_calls{0};
  std::atomic<int> store_calls{0};
  std::atomic<int> handler_calls{0};
  server.upload(
    "/api/files/upload",
    [&handler_calls](const HttpRequest&, UploadStreamContext&, HttpResponse& response) {
      ++handler_calls;
      response.set_status(201, "Created");
    },
    [&setup_calls, &store_calls](const HttpRequest&, UploadStreamContext& context, HttpParser&) {
      ++setup_calls;
      context.store_chunk_data = [&store_calls](std::string_view, const std::string&) {
        ++store_calls;
        return true;
      };
    },
    [&config](const HttpRequest& request, const UploadStreamContext& context) {
      return run_preflight(config, request, context);
    });

  ASSERT_TRUE(server.init());
  std::thread server_thread([&server]() { server.start(); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const auto response = send_raw(server.actual_port(),
                                 "POST /api/files/upload HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Authorization: Bearer normal-token\r\n"
                                 "Content-Disposition: attachment; filename=\"payload.bin\"\r\n"
                                 "Content-Length: 5\r\n"
                                 "Connection: close\r\n\r\n"
                                 "hello");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_NE(response.find("415 Unsupported Media Type"), std::string::npos) << response;
  EXPECT_NE(response.find("UNSUPPORTED_FILE_TYPE"), std::string::npos) << response;
  EXPECT_EQ(setup_calls.load(), 0);
  EXPECT_EQ(store_calls.load(), 0);
  EXPECT_EQ(handler_calls.load(), 0);
}

TEST(UploadPolicyHttpTest, RejectsEmptyAudioBeforeSetupAndHashing) {
  ServerConfig config;
  UploadAuthService auth;
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(auth);

  std::atomic<int> setup_calls{0};
  std::atomic<int> handler_calls{0};
  server.upload(
    "/api/files/upload",
    [&handler_calls](const HttpRequest&, UploadStreamContext&, HttpResponse&) { ++handler_calls; },
    [&setup_calls](const HttpRequest&, UploadStreamContext&, HttpParser&) { ++setup_calls; },
    [&config](const HttpRequest& request, const UploadStreamContext& context) {
      return run_preflight(config, request, context);
    });

  ASSERT_TRUE(server.init());
  std::thread server_thread([&server]() { server.start(); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const auto response = send_raw(server.actual_port(),
                                 "POST /api/files/upload HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Authorization: Bearer normal-token\r\n"
                                 "Content-Disposition: attachment; filename=\"empty.mp3\"\r\n"
                                 "Content-Length: 0\r\n"
                                 "Connection: close\r\n\r\n");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_NE(response.find("400 Bad Request"), std::string::npos) << response;
  EXPECT_NE(response.find("EMPTY_FILE"), std::string::npos) << response;
  EXPECT_EQ(setup_calls.load(), 0);
  EXPECT_EQ(handler_calls.load(), 0);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
