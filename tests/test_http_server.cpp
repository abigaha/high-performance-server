#include "auth_service.h"
#include "database_pool.h"
#include "http_server.h"
#include "mock_connection.h"
#include "pending_chunk_deletions.h"
#include "tcp_client.h"
#include "thread_pool.h"
#include "upload_policy.h"
#include "upload_setup.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using namespace hps;

// 模拟认证服务：已知 token "valid-token" → NORMAL，其余 → GUEST
class MockAuthService : public IAuthService {
public:
  TokenValidationResult validate_token(const std::string& token) override {
    if (token == "throw-token") {
      throw std::runtime_error("authentication storage failure");
    }
    if (token == "storage-error-token") {
      return {TokenValidationStatus::STORAGE_ERROR, {}, {}};
    }
    if (token == "valid-token") {
      return {TokenValidationStatus::AUTHENTICATED,
              EffectiveIdentity{1, "test", UserRole::NORMAL, VipStatus::NONE, std::nullopt},
              {}};
    }
    return {};
  }

  std::string generate_token(const AuthUser& user) override {
    static_cast<void>(user);
    return "mock-token";
  }

  AuthenticationResult authenticate(const std::string& username, const std::string& password) override {
    static_cast<void>(username);
    static_cast<void>(password);
    return {};
  }
};

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace {

std::string send_raw(uint16_t port, const std::string& raw);

class UploadSetupFileSystem final : public IFileSystem {
public:
  std::vector<FileChunk> split_file(const std::string& path, std::size_t chunk_size) override {
    static_cast<void>(path);
    static_cast<void>(chunk_size);
    return {};
  }

  std::string compute_file_hash(const std::string& path) override {
    static_cast<void>(path);
    return {};
  }

  std::string compute_chunk_hash(const FileChunk& chunk) override {
    static_cast<void>(chunk);
    return {};
  }

  bool store_file(const std::string& path, const std::vector<char>& data) override {
    static_cast<void>(path);
    static_cast<void>(data);
    active_uploads_during_store = coordinator->active_uploads();
    return true;
  }

  bool delete_file(const std::string& path) override {
    static_cast<void>(path);
    return false;
  }

  std::optional<std::vector<char>> read_file(const std::string& path) override {
    static_cast<void>(path);
    return std::nullopt;
  }

  ChunkLifecycleCoordinator* coordinator{nullptr};
  std::size_t active_uploads_during_store{0};
};

TEST(UploadSetupTest, ProductionFactoryOwnsGuardAcrossPhysicalStoreAndDatabaseFinalize) {
  MockConnection* connection = nullptr;
  DatabasePool database([&connection] {
    auto mock = std::make_unique<MockConnection>();
    connection = mock.get();
    return mock;
  });
  DbConfig config;
  config.pool_size = 1;
  ASSERT_TRUE(database.init(config));
  UploadSetupFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  file_system.coordinator = &coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));
  auto setup = make_upload_setup(database, file_system, coordinator);
  UploadStreamContext context;
  context.file_name = "track.mp3";
  HttpRequest request;
  HttpParser parser;

  EXPECT_EQ(coordinator.active_uploads(), 0U);
  setup(request, context, parser);
  EXPECT_EQ(coordinator.active_uploads(), 1U);
  ASSERT_TRUE(context.store_chunk_data("data", "hash"));
  EXPECT_EQ(file_system.active_uploads_during_store, 1U);
  connection->query_hook = [&coordinator](const std::string&, const std::vector<std::string>&) {
    EXPECT_EQ(coordinator.active_uploads(), 1U);
    return std::optional<QueryResult>{QueryResult{}};
  };
  EXPECT_FALSE(database.get_file_record_by_hash("file-hash"));
  context.chunk_lifecycle_guard.reset();
  EXPECT_EQ(coordinator.active_uploads(), 0U);
}

TEST(UploadSetupTest, ProductionFactoryRejectsCoordinatorDifferentFromDatabaseCanonical) {
  DatabasePool database;
  UploadSetupFileSystem file_system;
  ChunkLifecycleCoordinator canonical;
  ChunkLifecycleCoordinator other;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(canonical));

  EXPECT_THROW(static_cast<void>(make_upload_setup(database, file_system, other)), std::logic_error);
}

void wait_for_cleanup_waiter(const ChunkLifecycleCoordinator& coordinator) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (coordinator.cleanup_waiters() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  ASSERT_GT(coordinator.cleanup_waiters(), 0U);
}

TEST(UploadStreamContextTest, LifecycleGuardIsReleasedByContextDestructor) {
  DatabasePool database;
  UploadSetupFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  file_system.coordinator = &coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));
  auto setup = make_upload_setup(database, file_system, coordinator);
  std::atomic<bool> acquired{false};
  auto context = std::make_unique<UploadStreamContext>();
  context->file_name = "track.mp3";
  HttpRequest request;
  HttpParser parser;
  setup(request, *context, parser);

  auto future = std::async(std::launch::async, [&] {
    auto guard = coordinator.acquire_cleanup_guard();
    acquired.store(true);
  });

  wait_for_cleanup_waiter(coordinator);
  EXPECT_FALSE(acquired.load());
  context.reset();
  future.get();
  EXPECT_TRUE(acquired.load());
}

std::string valid_mp3_payload() {
  std::string payload(kAudioSignatureProbeSize, '\0');
  payload[0] = static_cast<char>(0xFF);
  payload[1] = static_cast<char>(0xFB);
  payload[2] = static_cast<char>(0x90);
  return payload;
}

std::string upload_request(const std::string& payload) {
  return "POST /upload HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer valid-token\r\n"
         "Content-Disposition: attachment; filename=\"track.mp3\"\r\nContent-Length: " +
         std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
}

void wait_for_no_active_uploads(const ChunkLifecycleCoordinator& coordinator) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (coordinator.active_uploads() != 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  ASSERT_EQ(coordinator.active_uploads(), 0U);
}

TEST(UploadSetupTest, ProductionSetupReleasesGuardAfterSignatureRejection) {
  MockAuthService auth;
  DatabasePool database;
  UploadSetupFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  file_system.coordinator = &coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));
  auto setup = make_upload_setup(database, file_system, coordinator);
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(auth);
  server.upload("/upload", [](const HttpRequest&, UploadStreamContext&, HttpResponse&) {}, setup);
  ASSERT_TRUE(server.init());
  std::thread thread([&server] { server.start(); });

  const auto response = send_raw(server.actual_port(), upload_request(std::string(kAudioSignatureProbeSize, 'x')));
  wait_for_no_active_uploads(coordinator);
  server.stop();
  thread.join();

  EXPECT_NE(response.find("415 Unsupported Media Type"), std::string::npos) << response;
}

TEST(UploadSetupTest, ProductionSetupReleasesGuardAfterHandlerException) {
  MockAuthService auth;
  DatabasePool database;
  UploadSetupFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  file_system.coordinator = &coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));
  auto setup = make_upload_setup(database, file_system, coordinator);
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(auth);
  server.upload(
    "/upload",
    [](const HttpRequest&, UploadStreamContext&, HttpResponse&) { throw std::runtime_error("failed"); },
    setup);
  ASSERT_TRUE(server.init());
  std::thread thread([&server] { server.start(); });

  const auto response = send_raw(server.actual_port(), upload_request(valid_mp3_payload()));
  wait_for_no_active_uploads(coordinator);
  server.stop();
  thread.join();

  EXPECT_NE(response.find("500 Internal Server Error"), std::string::npos) << response;
}

TEST(UploadSetupTest, ProductionSetupReleasesGuardWhenRequestStateResets) {
  MockAuthService auth;
  DatabasePool database;
  UploadSetupFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  file_system.coordinator = &coordinator;
  ASSERT_TRUE(database.bind_chunk_lifecycle_coordinator(coordinator));
  auto setup = make_upload_setup(database, file_system, coordinator);
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(auth);
  server.upload(
    "/upload",
    [&coordinator](const HttpRequest&, UploadStreamContext&, HttpResponse& response) {
      EXPECT_EQ(coordinator.active_uploads(), 1U);
      response.set_status(201, "Created");
    },
    setup);
  ASSERT_TRUE(server.init());
  std::thread thread([&server] { server.start(); });

  const auto response = send_raw(server.actual_port(), upload_request(valid_mp3_payload()));
  wait_for_no_active_uploads(coordinator);
  server.stop();
  thread.join();

  EXPECT_NE(response.find("201 Created"), std::string::npos) << response;
}

TEST(HttpServerTest, AuthenticationExceptionRemainsAStorageErrorInsideConnectionHandling) {
  MockAuthService auth;
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(auth);
  std::atomic<int> received_status{static_cast<int>(TokenValidationStatus::INVALID)};
  server.get("/protected", [&received_status](const HttpRequest& request, HttpResponse& response) {
    received_status.store(static_cast<int>(request.auth_status));
    response.set_status(200, "OK");
    response.body = "handled";
    response.set_content_length(response.body.size());
  });
  ASSERT_TRUE(server.init());
  std::thread thread([&server]() { server.start(); });
  thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const auto response = send_raw(server.actual_port(),
                                 "GET /protected HTTP/1.1\r\nHost: localhost\r\n"
                                 "Authorization: Bearer throw-token\r\nConnection: close\r\n\r\n");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_NE(response.find("200 OK"), std::string::npos) << response;
  EXPECT_EQ(received_status.load(), static_cast<int>(TokenValidationStatus::STORAGE_ERROR));
}

// 辅助：发送原始 HTTP 请求并读取响应
std::string send_raw(uint16_t port, const std::string& raw) {
  TcpClient client("127.0.0.1", port);
  if (!client.connect_to_server()) {
    return "";
  }
  if (!client.send_message(raw)) {
    return "";
  }
  std::string resp;
  client.receive_message(resp, ReadMode::RAW, 2000);
  return resp;
}

} // namespace

// TH1: GET 基本请求
TEST(HttpServerTest, GetBasic) {
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.get("/hello", [](const HttpRequest&, HttpResponse& resp) {
    resp.set_status(200, "OK");
    resp.set_content_type("text/plain");
    resp.body = "world";
    resp.set_content_length(resp.body.size());
  });

  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  auto resp = send_raw(port, "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp.find("200 OK"), std::string::npos) << "响应: " << resp;
  ASSERT_NE(resp.find("world"), std::string::npos) << "响应: " << resp;
}

// TH2: 参数路由
TEST(HttpServerTest, ParamRoute) {
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.get("/song/:id", [](const HttpRequest& req, HttpResponse& resp) {
    resp.set_status(200, "OK");
    resp.set_content_type("text/plain");
    resp.body = "song-" + req.path_params.at("id");
    resp.set_content_length(resp.body.size());
  });

  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  auto resp = send_raw(port, "GET /song/42 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp.find("200 OK"), std::string::npos) << "响应: " << resp;
  ASSERT_NE(resp.find("song-42"), std::string::npos) << "响应: " << resp;
}

// TH3: 404 未找到
TEST(HttpServerTest, NotFound) {
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  auto resp = send_raw(port, "GET /nope HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp.find("404 Not Found"), std::string::npos) << "响应: " << resp;
}

// TH4: POST 带 body
TEST(HttpServerTest, PostBody) {
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.post("/echo", [](const HttpRequest& req, HttpResponse& resp) {
    resp.set_status(200, "OK");
    resp.set_content_type("text/plain");
    resp.body = req.body;
    resp.set_content_length(resp.body.size());
  });

  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  auto resp =
    send_raw(port, "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp.find("200 OK"), std::string::npos) << "响应: " << resp;
  ASSERT_NE(resp.find("hello"), std::string::npos) << "响应: " << resp;
}

// TH5: Keep-Alive 同连接多请求
TEST(HttpServerTest, KeepAlive) {
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.get("/ping", [](const HttpRequest&, HttpResponse& resp) {
    resp.set_status(200, "OK");
    resp.set_content_type("text/plain");
    resp.body = "pong";
    resp.set_content_length(resp.body.size());
  });

  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  // 同连接发两个请求
  TcpClient client("127.0.0.1", port);
  ASSERT_TRUE(client.connect_to_server());
  ASSERT_TRUE(client.send_message("GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n"));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  std::string resp1;
  client.receive_message(resp1, ReadMode::RAW, 2000);
  ASSERT_NE(resp1.find("pong"), std::string::npos) << "第一次响应: " << resp1;

  ASSERT_TRUE(client.send_message("GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  std::string resp2;
  client.receive_message(resp2, ReadMode::RAW, 2000);

  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp2.find("pong"), std::string::npos) << "第二次响应: " << resp2;
}

// TH6: 畸形请求 400
TEST(HttpServerTest, Malformed) {
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  auto resp = send_raw(port, "GARBAGE\r\n\r\n");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp.find("400 Bad Request"), std::string::npos) << "响应: " << resp;
}

// TH8: upload 未鉴权 → handler 仍被调用（因 file_name 已设置），但不触发流式 setup
TEST(HttpServerTest, UploadNoAuth) {
  MockAuthService mock_auth;
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(mock_auth);

  std::atomic<bool> setup_called{false};
  server.upload(
    "/upload",
    [](const HttpRequest&, const UploadStreamContext& ctx, HttpResponse& resp) {
      EXPECT_FALSE(ctx.file_name.empty());
      resp.set_status(401, "Unauthorized");
      resp.body = "auth required";
      resp.set_content_length(resp.body.size());
    },
    [&setup_called](const HttpRequest&, UploadStreamContext&, HttpParser&) { setup_called.store(true); });

  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  auto resp = send_raw(port,
                       "POST /upload HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "Content-Disposition: attachment; filename=\"test.bin\"\r\n"
                       "Content-Length: 5\r\n"
                       "Connection: close\r\n\r\n"
                       "hello");

  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_FALSE(setup_called.load()) << "未鉴权不应触发流式 setup";
  ASSERT_NE(resp.find("401 Unauthorized"), std::string::npos) << "响应: " << resp;
  ASSERT_NE(resp.find("401 Unauthorized"), std::string::npos) << "响应: " << resp;
  ASSERT_NE(resp.find("AUTH_REQUIRED"), std::string::npos) << "响应: " << resp;
}

// TH9: upload 已鉴权 → 流式 setup 被触发，handler 正常处理
TEST(HttpServerTest, UploadWithAuth) {
  MockAuthService mock_auth;
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(mock_auth);

  std::atomic<bool> setup_called{false};
  server.upload(
    "/upload",
    [](const HttpRequest&, const UploadStreamContext& ctx, HttpResponse& resp) {
      EXPECT_FALSE(ctx.file_name.empty());
      resp.set_status(200, "OK");
      resp.body = "uploaded";
      resp.set_content_length(resp.body.size());
    },
    [&setup_called](const HttpRequest&, UploadStreamContext&, HttpParser&) { setup_called.store(true); });

  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  auto resp = send_raw(port,
                       "POST /upload HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Authorization: Bearer valid-token\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "Content-Disposition: attachment; filename=\"test.bin\"\r\n"
                       "Content-Length: 5\r\n"
                       "Connection: close\r\n\r\n"
                       "hello");

  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_TRUE(setup_called.load()) << "已鉴权应触发流式 setup";
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << "响应: " << resp;
  ASSERT_NE(resp.find("uploaded"), std::string::npos) << "响应: " << resp;
}

// TH9-A: Content-Disposition 优先使用 RFC 5987 文件名，并安全回退普通文件名
TEST(HttpServerTest, ParsesContentDispositionFilenameVariants) {
  const auto extended = parse_content_disposition_filename(
    "attachment; filename=\"fallback.mp3\"; filename*=UTF-8''%E4%B8%AD%E6%96%87%20%22demo%22.MP3");
  ASSERT_TRUE(extended.has_value());
  EXPECT_EQ(*extended, "中文 \"demo\".MP3");

  const auto fallback =
    parse_content_disposition_filename("attachment; filename=\"fallback.mp3\"; filename*=UTF-8''bad%ZZname.mp3");
  ASSERT_TRUE(fallback.has_value());
  EXPECT_EQ(*fallback, "fallback.mp3");

  const auto sanitized = parse_content_disposition_filename("attachment; filename=\"../../safe.ogg\"");
  ASSERT_TRUE(sanitized.has_value());
  EXPECT_EQ(*sanitized, "safe.ogg");

  EXPECT_FALSE(parse_content_disposition_filename("attachment; filename*=UTF-8''bad%2").has_value());
}

// TH9-B: 上传头与 body 跨两次 socket 读取时保持同一上传上下文
TEST(HttpServerTest, UploadContextSurvivesSplitSocketReads) {
  MockAuthService mock_auth;
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(mock_auth);

  server.upload(
    "/upload",
    [](const HttpRequest&, const UploadStreamContext& context, HttpResponse& response) {
      response.set_status(200, "OK");
      response.body =
        context.file_name + "|" + std::to_string(context.chunks.size()) + "|" + std::to_string(context.total_size);
      response.set_content_length(response.body.size());
    },
    [](const HttpRequest&, UploadStreamContext& context, HttpParser&) {
      context.store_chunk_data = [](std::string_view, const std::string&) { return true; };
    });

  ASSERT_TRUE(server.init());
  std::thread server_thread([&server]() { server.start(); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  TcpClient client("127.0.0.1", server.actual_port());
  ASSERT_TRUE(client.connect_to_server());
  ASSERT_TRUE(client.send_message("POST /upload HTTP/1.1\r\n"
                                  "Host: localhost\r\n"
                                  "Authorization: Bearer valid-token\r\n"
                                  "Content-Disposition: attachment; filename=\"fallback.mp3\"; "
                                  "filename*=UTF-8''%E4%B8%AD%E6%96%87.mp3\r\n"
                                  "Content-Length: 5\r\n"
                                  "Connection: close\r\n\r\n"));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ASSERT_TRUE(client.send_message("hello"));

  std::string response;
  client.receive_message(response, ReadMode::RAW, 3000);
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_NE(response.find("200 OK"), std::string::npos) << response;
  EXPECT_NE(response.find("中文.mp3|1|5"), std::string::npos) << response;
}

// TH9-C: 跨域预检允许浏览器发送 Content-Disposition
TEST(HttpServerTest, CorsAllowsContentDispositionRequestHeader) {
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  ASSERT_TRUE(server.init());
  std::thread server_thread([&server]() { server.start(); });
  server_thread.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const auto response = send_raw(server.actual_port(),
                                 "OPTIONS /api/files/upload HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Access-Control-Request-Method: POST\r\n"
                                 "Access-Control-Request-Headers: Content-Disposition\r\n"
                                 "Connection: close\r\n\r\n");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_NE(response.find("204 No Content"), std::string::npos) << response;
  EXPECT_NE(response.find("Content-Disposition"), std::string::npos) << response;
}

// TH10: handler 异常返回 500
TEST(HttpServerTest, HandlerException) {
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.get("/boom", [](const HttpRequest&, HttpResponse&) { throw std::runtime_error("boom!"); });

  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  auto resp = send_raw(port, "GET /boom HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp.find("500 Internal Server Error"), std::string::npos) << "响应: " << resp;
}

// TH11: 普通路由接收有效 Bearer token 对应的认证用户
TEST(HttpServerTest, AuthenticatedRouteReceivesAuthUser) {
  MockAuthService mock_auth;
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(mock_auth);

  std::atomic<int64_t> received_user_id{0};
  std::atomic<int> received_role{static_cast<int>(UserRole::GUEST)};
  server.get("/me", [&received_user_id, &received_role](const HttpRequest& req, HttpResponse& resp) {
    received_user_id.store(req.auth_user.user_id);
    received_role.store(static_cast<int>(req.auth_user.role));
    resp.set_status(200, "OK");
    resp.body = "authenticated";
    resp.set_content_length(resp.body.size());
  });

  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  auto resp = send_raw(port,
                       "GET /me HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Authorization: Bearer valid-token\r\n"
                       "Connection: close\r\n\r\n");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp.find("200 OK"), std::string::npos) << "响应: " << resp;
  EXPECT_EQ(received_user_id.load(), 1);
  EXPECT_EQ(received_role.load(), static_cast<int>(UserRole::NORMAL));
}

// TH12: 普通路由将无效 Bearer token 视为访客
TEST(HttpServerTest, InvalidTokenRouteReceivesGuestUser) {
  MockAuthService mock_auth;
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(mock_auth);

  std::atomic<int64_t> received_user_id{-1};
  std::atomic<int> received_role{static_cast<int>(UserRole::NORMAL)};
  server.get("/me", [&received_user_id, &received_role](const HttpRequest& req, HttpResponse& resp) {
    received_user_id.store(req.auth_user.user_id);
    received_role.store(static_cast<int>(req.auth_user.role));
    resp.set_status(200, "OK");
    resp.body = "guest";
    resp.set_content_length(resp.body.size());
  });

  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  auto resp = send_raw(port,
                       "GET /me HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Authorization: Bearer invalid-token\r\n"
                       "Connection: close\r\n\r\n");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp.find("200 OK"), std::string::npos) << "响应: " << resp;
  EXPECT_EQ(received_user_id.load(), 0);
  EXPECT_EQ(received_role.load(), static_cast<int>(UserRole::GUEST));
}
