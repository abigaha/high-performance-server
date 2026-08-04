#include "auth_service.h"
#include "http_server.h"
#include "tcp_client.h"
#include "upload_policy.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace hps;

namespace {

std::string send_raw(uint16_t port, const std::string& request);

class UploadAuthService : public IAuthService {
public:
  TokenValidationResult validate_token(const std::string& token) override {
    if (token == "guest-token") {
      return {TokenValidationStatus::AUTHENTICATED, {0, "guest", UserRole::GUEST, VipStatus::NONE, std::nullopt}, {}};
    }
    if (token == "normal-token") {
      return {TokenValidationStatus::AUTHENTICATED, {1, "normal", UserRole::NORMAL, VipStatus::NONE, std::nullopt}, {}};
    }
    if (token == "vip-token") {
      return {TokenValidationStatus::AUTHENTICATED,
              {2, "vip", UserRole::VIP, VipStatus::ACTIVE, std::chrono::system_clock::time_point::max()},
              {}};
    }
    if (token == "admin-token") {
      return {TokenValidationStatus::AUTHENTICATED, {3, "admin", UserRole::ADMIN, VipStatus::NONE, std::nullopt}, {}};
    }
    if (token == "storage-error-token") {
      return {TokenValidationStatus::STORAGE_ERROR, {}, {}};
    }
    return {};
  }

  std::string generate_token(const AuthUser& user) override {
    static_cast<void>(user);
    return "token";
  }

  AuthenticationResult authenticate(const std::string& username, const std::string& password) override {
    static_cast<void>(username);
    static_cast<void>(password);
    return {};
  }
};

TEST(UploadPolicyHttpTest, HeaderAuthenticationUsesCapabilityAndMapsStorageError) {
  struct Case {
    std::string token;
    int expected_status;
    bool expects_setup;
    std::string expected_code;
  };

  const std::array cases = {
    Case{"guest-token", 401, false, "AUTH_REQUIRED"},
    Case{"normal-token", 201, true, ""},
    Case{"vip-token", 201, true, ""},
    Case{"admin-token", 201, true, ""},
    Case{"storage-error-token", 500, false, "PERSISTENCE_ERROR"},
  };

  for (const auto& test_case : cases) {
    UploadAuthService auth;
    HttpServer server(TcpServer::Config{0, 128, 2, 50});
    server.set_auth_service(auth);
    std::atomic<int> setup_calls{0};
    server.upload(
      "/upload-auth",
      [](const HttpRequest&, UploadStreamContext&, HttpResponse& response) { response.set_status(201, "Created"); },
      [&setup_calls](const HttpRequest&, UploadStreamContext& context, HttpParser&) {
        ++setup_calls;
        context.store_chunk_data = [](std::string_view, const std::string&) { return true; };
      });
    ASSERT_TRUE(server.init());
    std::thread server_thread([&server]() { server.start(); });
    server_thread.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto response = send_raw(server.actual_port(),
                                   "POST /upload-auth HTTP/1.1\r\n"
                                   "Host: localhost\r\n"
                                   "Authorization: Bearer " +
                                     test_case.token +
                                     "\r\nContent-Disposition: attachment; filename=\"payload.bin\"\r\n"
                                     "Content-Length: 1\r\nConnection: close\r\n\r\nx");
    server.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_NE(response.find(std::to_string(test_case.expected_status)), std::string::npos) << test_case.token;
    EXPECT_EQ(setup_calls.load() > 0, test_case.expects_setup) << test_case.token;
    if (!test_case.expected_code.empty()) {
      EXPECT_NE(response.find(test_case.expected_code), std::string::npos) << response;
    }
  }
}

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

std::optional<HttpResponse> run_signature_probe(std::string_view file_name, std::string_view prefix) {
  const auto validation = validate_audio_signature(file_name, AudioSignaturePrefix{prefix});
  if (validation.accepted) {
    return std::nullopt;
  }
  return make_upload_validation_response(validation);
}

void append_le16(std::string& value, std::uint16_t number) {
  value.push_back(static_cast<char>(number & 0xFFU));
  value.push_back(static_cast<char>((number >> 8U) & 0xFFU));
}

void append_le32(std::string& value, std::uint32_t number) {
  value.push_back(static_cast<char>(number & 0xFFU));
  value.push_back(static_cast<char>((number >> 8U) & 0xFFU));
  value.push_back(static_cast<char>((number >> 16U) & 0xFFU));
  value.push_back(static_cast<char>((number >> 24U) & 0xFFU));
}

void append_be32(std::string& value, std::uint32_t number) {
  value.push_back(static_cast<char>((number >> 24U) & 0xFFU));
  value.push_back(static_cast<char>((number >> 16U) & 0xFFU));
  value.push_back(static_cast<char>((number >> 8U) & 0xFFU));
  value.push_back(static_cast<char>(number & 0xFFU));
}

std::string mp3_signature() {
  return std::string({static_cast<char>(0xFF), static_cast<char>(0xFB), static_cast<char>(0x90), '\0'});
}

std::string ogg_signature() {
  std::string value{"OggS"};
  value.append(22, '\0');
  value.push_back('\x01');
  value.push_back('\x1E');
  value.append("\x01vorbis", 7);
  append_le32(value, 0U);
  value.push_back('\x01');
  append_le32(value, 44100U);
  value.append(12, '\0');
  value.push_back(static_cast<char>(0xB8));
  value.push_back('\x01');
  return value;
}

std::string wav_header() {
  std::string value{"RIFF"};
  append_le32(value, 0U);
  value += "WAVEfmt ";
  append_le32(value, 16U);
  append_le16(value, 1U);
  append_le16(value, 1U);
  append_le32(value, 44100U);
  append_le32(value, 44100U);
  append_le16(value, 1U);
  append_le16(value, 8U);
  return value;
}

void set_wav_riff_size(std::string& value) {
  const auto riff_size = static_cast<std::uint32_t>(value.size() - 8U);
  value[4] = static_cast<char>(riff_size & 0xFFU);
  value[5] = static_cast<char>((riff_size >> 8U) & 0xFFU);
  value[6] = static_cast<char>((riff_size >> 16U) & 0xFFU);
  value[7] = static_cast<char>((riff_size >> 24U) & 0xFFU);
}

std::string wav_signature() {
  auto value = wav_header();
  value += "data";
  append_le32(value, 1U);
  value.push_back('\0');
  value.push_back('\0');
  set_wav_riff_size(value);
  return value;
}

std::string wav_with_optional_chunks_signature() {
  auto value = wav_header();
  value += "JUNK";
  append_le32(value, 1U);
  value += "x";
  value.push_back('\0');
  value += "LIST";
  append_le32(value, 2U);
  value += "IN";
  value += "data";
  append_le32(value, 1U);
  value.push_back('\0');
  value.push_back('\0');
  set_wav_riff_size(value);
  return value;
}

std::string wav_with_truncated_optional_chunk_signature() {
  auto value = wav_header();
  value += "JUNK";
  append_le32(value, 1U);
  value += "x";
  set_wav_riff_size(value);
  return value;
}

std::string wav_with_out_of_bounds_optional_chunk_signature() {
  auto value = wav_header();
  value += "LIST";
  append_le32(value, 0xFFFFFFFEU);
  value += "data";
  append_le32(value, 1U);
  value.push_back('\0');
  value.push_back('\0');
  set_wav_riff_size(value);
  return value;
}

std::string flac_signature() {
  std::string value{"fLaC"};
  value.push_back(static_cast<char>(0x80));
  value.append(2, '\0');
  value.push_back('\x22');
  value.push_back('\0');
  value.push_back('\x10');
  value.push_back('\0');
  value.push_back('\x10');
  value.append(6, '\0');
  value.push_back('\x0A');
  value.push_back(static_cast<char>(0xC4));
  value.push_back('\x40');
  value.push_back(static_cast<char>(0xF0));
  value.append(4, '\0');
  value.append(16, '\0');
  return value;
}

std::string aac_signature() {
  return std::string({static_cast<char>(0xFF),
                      static_cast<char>(0xF1),
                      static_cast<char>(0x50),
                      static_cast<char>(0x80),
                      '\0',
                      static_cast<char>(0xE0),
                      static_cast<char>(0xFC)});
}

std::string m4a_signature() {
  std::string value;
  append_be32(value, 20U);
  value += "ftypM4A ";
  append_be32(value, 0U);
  value += "isom";
  return value;
}

std::string wma_signature() {
  std::string value({static_cast<char>(0x30),
                     static_cast<char>(0x26),
                     static_cast<char>(0xB2),
                     static_cast<char>(0x75),
                     static_cast<char>(0x8E),
                     static_cast<char>(0x66),
                     static_cast<char>(0xCF),
                     static_cast<char>(0x11),
                     static_cast<char>(0xA6),
                     static_cast<char>(0xD9),
                     static_cast<char>(0x00),
                     static_cast<char>(0xAA),
                     static_cast<char>(0x00),
                     static_cast<char>(0x62),
                     static_cast<char>(0xCE),
                     static_cast<char>(0x6C)});
  append_le32(value, 30U);
  append_le32(value, 0U);
  append_le32(value, 1U);
  value.push_back('\x01');
  value.push_back('\x02');
  return value;
}

std::string ape_signature() {
  std::string value{"MAC "};
  append_le16(value, 3980U);
  append_le16(value, 0U);
  append_le32(value, 52U);
  append_le32(value, 24U);
  append_le32(value, 0U);
  append_le32(value, 0U);
  append_le32(value, 1U);
  append_le32(value, 0U);
  append_le32(value, 0U);
  value.append(16, '\0');
  return value;
}

std::string opus_signature() {
  std::string value{"OggS"};
  value.append(22, '\0');
  value.push_back('\x01');
  value.push_back('\x13');
  value += "OpusHead";
  value.push_back('\x01');
  value.push_back('\x01');
  value.append(9, '\0');
  return value;
}

struct AudioSignatureCase {
  std::string file_name;
  std::string prefix;
};

std::array<AudioSignatureCase, 9> minimum_audio_signature_cases() {
  return {{{"track.mp3", mp3_signature()},
           {"track.ogg", ogg_signature()},
           {"track.wav", wav_signature()},
           {"track.flac", flac_signature()},
           {"track.aac", aac_signature()},
           {"track.m4a", m4a_signature()},
           {"track.wma", wma_signature()},
           {"track.ape", ape_signature()},
           {"track.opus", opus_signature()}}};
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

TEST(UploadPolicyTest, AppliesNormalLimitToAdminInclusively) {
  ServerConfig config;
  config.normal_max_size = 10 * 1024 * 1024;
  config.vip_max_size = 100 * 1024 * 1024;

  const auto at_limit =
    validate_audio_upload("track.mp3", static_cast<std::size_t>(config.normal_max_size), UserRole::ADMIN, config);
  EXPECT_TRUE(at_limit.accepted);
  EXPECT_EQ(at_limit.max_size, static_cast<std::size_t>(config.normal_max_size));

  const auto over_limit =
    validate_audio_upload("track.mp3", static_cast<std::size_t>(config.normal_max_size) + 1, UserRole::ADMIN, config);
  EXPECT_FALSE(over_limit.accepted);
  EXPECT_EQ(over_limit.status_code, 413);
  EXPECT_EQ(over_limit.code, "FILE_TOO_LARGE");
  EXPECT_EQ(over_limit.max_size, static_cast<std::size_t>(config.normal_max_size));
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

TEST(UploadPolicyTest, ValidatesMinimumAudioSignaturesForEveryAllowedExtension) {
  const auto cases = minimum_audio_signature_cases();

  for (const auto& [file_name, prefix] : cases) {
    const auto validation = validate_audio_signature(file_name, AudioSignaturePrefix{prefix});
    EXPECT_TRUE(validation.accepted) << file_name;
    EXPECT_EQ(validation.content_type, *audio_content_type(file_name)) << file_name;
  }
}

TEST(UploadPolicyTest, RejectsTruncatedAndWrongAudioSignaturesForEveryAllowedExtension) {
  const auto cases = minimum_audio_signature_cases();

  for (const auto& [file_name, prefix] : cases) {
    ASSERT_GT(prefix.size(), 1U) << file_name;
    const auto truncated =
      validate_audio_signature(file_name, AudioSignaturePrefix{prefix.substr(0, prefix.size() - 1)});
    EXPECT_FALSE(truncated.accepted) << file_name;
    EXPECT_EQ(truncated.status_code, 415) << file_name;
    EXPECT_EQ(truncated.code, "INVALID_AUDIO_SIGNATURE") << file_name;

    auto wrong_prefix = prefix;
    wrong_prefix[0] = static_cast<char>(static_cast<unsigned char>(wrong_prefix[0]) ^ 0xFFU);
    const auto wrong = validate_audio_signature(file_name, AudioSignaturePrefix{wrong_prefix});
    EXPECT_FALSE(wrong.accepted) << file_name;
    EXPECT_EQ(wrong.status_code, 415) << file_name;
    EXPECT_EQ(wrong.code, "INVALID_AUDIO_SIGNATURE") << file_name;
  }

  auto wrong_prefix = mp3_signature();
  wrong_prefix[0] = static_cast<char>(static_cast<unsigned char>(wrong_prefix[0]) ^ 0xFFU);
  const auto response =
    make_upload_validation_response(validate_audio_signature("track.mp3", AudioSignaturePrefix{wrong_prefix}));
  const auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(response.status_code, 415);
  EXPECT_EQ(body.at("code"), "INVALID_AUDIO_SIGNATURE");
}

TEST(UploadPolicyTest, AcceptsWavWithAlignedJunkAndListChunks) {
  const auto validation =
    validate_audio_signature("track.wav", AudioSignaturePrefix{wav_with_optional_chunks_signature()});

  EXPECT_TRUE(validation.accepted);
  EXPECT_EQ(validation.status_code, 200);
  EXPECT_EQ(validation.content_type, "audio/wav");
}

TEST(UploadPolicyTest, RejectsTruncatedAndOutOfBoundsWavOptionalChunks) {
  const std::array<std::pair<std::string_view, std::string>, 2> cases{{
    {"truncated-junk-padding", wav_with_truncated_optional_chunk_signature()},
    {"out-of-bounds-list-length", wav_with_out_of_bounds_optional_chunk_signature()},
  }};

  for (const auto& [case_name, prefix] : cases) {
    const auto validation = validate_audio_signature("track.wav", AudioSignaturePrefix{prefix});
    EXPECT_FALSE(validation.accepted) << case_name;
    EXPECT_EQ(validation.status_code, 415) << case_name;
    EXPECT_EQ(validation.code, "INVALID_AUDIO_SIGNATURE") << case_name;
  }
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

TEST(UploadPolicyHttpTest, RejectsInvalidSignatureBeforeStorageAndHandler) {
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
      const auto file_name = context.file_name;
      context.set_initial_chunk_probe(kAudioSignatureProbeSize, [file_name](std::string_view prefix) {
        return run_signature_probe(file_name, prefix);
      });
    },
    [&config](const HttpRequest& request, const UploadStreamContext& context) {
      return run_preflight(config, request, context);
    });

  ASSERT_TRUE(server.init());
  std::thread server_thread([&server]() { server.start(); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const std::string payload(kAudioSignatureProbeSize + 17, 'x');
  const auto response = send_raw(server.actual_port(),
                                 "POST /api/files/upload HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Authorization: Bearer normal-token\r\n"
                                 "Content-Disposition: attachment; filename=\"payload.mp3\"\r\n"
                                 "Content-Length: " +
                                   std::to_string(payload.size()) +
                                   "\r\n"
                                   "Connection: close\r\n\r\n" +
                                   payload);
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_NE(response.find("415 Unsupported Media Type"), std::string::npos) << response;
  EXPECT_NE(response.find("INVALID_AUDIO_SIGNATURE"), std::string::npos) << response;
  EXPECT_EQ(setup_calls.load(), 1);
  EXPECT_EQ(store_calls.load(), 0);
  EXPECT_EQ(handler_calls.load(), 0);
}

TEST(UploadPolicyHttpTest, RejectsTruncatedOggBeforeStorageAndHandler) {
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
      const auto file_name = context.file_name;
      context.set_initial_chunk_probe(kAudioSignatureProbeSize, [file_name](std::string_view prefix) {
        return run_signature_probe(file_name, prefix);
      });
    },
    [&config](const HttpRequest& request, const UploadStreamContext& context) {
      return run_preflight(config, request, context);
    });

  ASSERT_TRUE(server.init());
  std::thread server_thread([&server]() { server.start(); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const std::string payload{"OggS"};
  const auto response = send_raw(server.actual_port(),
                                 "POST /api/files/upload HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Authorization: Bearer normal-token\r\n"
                                 "Content-Disposition: attachment; filename=\"payload.ogg\"\r\n"
                                 "Content-Length: " +
                                   std::to_string(payload.size()) +
                                   "\r\n"
                                   "Connection: close\r\n\r\n" +
                                   payload);
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_NE(response.find("415 Unsupported Media Type"), std::string::npos) << response;
  EXPECT_NE(response.find("INVALID_AUDIO_SIGNATURE"), std::string::npos) << response;
  EXPECT_EQ(setup_calls.load(), 1);
  EXPECT_EQ(store_calls.load(), 0);
  EXPECT_EQ(handler_calls.load(), 0);
}

TEST(UploadPolicyHttpTest, DiscardsRemainingChunkedBodyAfterInvalidSignature) {
  UploadAuthService auth;
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(auth);

  std::atomic<int> store_calls{0};
  std::atomic<int> handler_calls{0};
  server.upload(
    "/upload",
    [&handler_calls](const HttpRequest&, UploadStreamContext&, HttpResponse& response) {
      ++handler_calls;
      response.set_status(201, "Created");
    },
    [&store_calls](const HttpRequest&, UploadStreamContext& context, HttpParser&) {
      context.store_chunk_data = [&store_calls](std::string_view, const std::string&) {
        ++store_calls;
        return true;
      };
      const auto file_name = context.file_name;
      context.set_initial_chunk_probe(kAudioSignatureProbeSize, [file_name](std::string_view prefix) {
        return run_signature_probe(file_name, prefix);
      });
    });

  ASSERT_TRUE(server.init());
  std::thread server_thread([&server]() { server.start(); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const std::string payload(kAudioSignatureProbeSize * 2, 'x');
  const auto response = send_raw(server.actual_port(),
                                 "POST /upload HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Authorization: Bearer normal-token\r\n"
                                 "Content-Disposition: attachment; filename=\"payload.mp3\"\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "Connection: close\r\n\r\n"
                                 "400\r\n" +
                                   payload + "\r\n0\r\n\r\n");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_NE(response.find("415 Unsupported Media Type"), std::string::npos) << response;
  EXPECT_NE(response.find("INVALID_AUDIO_SIGNATURE"), std::string::npos) << response;
  EXPECT_EQ(store_calls.load(), 0);
  EXPECT_EQ(handler_calls.load(), 0);
}

TEST(UploadPolicyHttpTest, PreservesValidBodyAfterInitialSignatureProbe) {
  ServerConfig config;
  UploadAuthService auth;
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(auth);

  std::mutex stored_mutex;
  std::string stored_data;
  std::atomic<int> handler_calls{0};
  server.upload(
    "/api/files/upload",
    [&handler_calls](const HttpRequest&, const UploadStreamContext& context, HttpResponse& response) {
      ++handler_calls;
      response.set_status(201, "Created");
      response.body = std::to_string(context.total_size);
      response.set_content_length(response.body.size());
    },
    [&stored_mutex, &stored_data](const HttpRequest&, UploadStreamContext& context, HttpParser&) {
      context.store_chunk_data = [&stored_mutex, &stored_data](std::string_view data, const std::string&) {
        std::lock_guard lock(stored_mutex);
        stored_data.append(data);
        return true;
      };
      const auto file_name = context.file_name;
      context.set_initial_chunk_probe(kAudioSignatureProbeSize, [file_name](std::string_view prefix) {
        return run_signature_probe(file_name, prefix);
      });
    },
    [&config](const HttpRequest& request, const UploadStreamContext& context) {
      return run_preflight(config, request, context);
    });

  ASSERT_TRUE(server.init());
  std::thread server_thread([&server]() { server.start(); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::string payload = wav_signature();
  payload.resize(kAudioSignatureProbeSize + 17, '\x5A');
  const auto response = send_raw(server.actual_port(),
                                 "POST /api/files/upload HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Authorization: Bearer normal-token\r\n"
                                 "Content-Disposition: attachment; filename=\"payload.wav\"\r\n"
                                 "Content-Length: " +
                                   std::to_string(payload.size()) +
                                   "\r\n"
                                   "Connection: close\r\n\r\n" +
                                   payload);
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string actual_data;
  {
    std::lock_guard lock(stored_mutex);
    actual_data = stored_data;
  }
  EXPECT_NE(response.find("201 Created"), std::string::npos) << response;
  EXPECT_NE(response.find(std::to_string(payload.size())), std::string::npos) << response;
  EXPECT_EQ(handler_calls.load(), 1);
  EXPECT_EQ(actual_data, payload);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
