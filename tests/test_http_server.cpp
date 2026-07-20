#include "auth_service.h"
#include "http_server.h"
#include "tcp_client.h"
#include "thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

using namespace hps;

// 模拟认证服务：已知 token "valid-token" → NORMAL，其余 → GUEST
class MockAuthService : public IAuthService {
public:
  AuthUser validate_token(const std::string& token) override {
    if (token == "valid-token") {
      return AuthUser{1, "test", UserRole::NORMAL};
    }
    return AuthUser{};
  }

  std::string generate_token(const AuthUser& user) override {
    static_cast<void>(user);
    return "mock-token";
  }

  std::optional<AuthUser> authenticate(const std::string& username, const std::string& password) override {
    static_cast<void>(username);
    static_cast<void>(password);
    return std::nullopt;
  }
};

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace {

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
    [](const HttpRequest&, UploadStreamContext& ctx, HttpResponse& resp) {
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
  ASSERT_NE(resp.find("auth required"), std::string::npos) << "响应: " << resp;
}

// TH9: upload 已鉴权 → 流式 setup 被触发，handler 正常处理
TEST(HttpServerTest, UploadWithAuth) {
  MockAuthService mock_auth;
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.set_auth_service(mock_auth);

  std::atomic<bool> setup_called{false};
  server.upload(
    "/upload",
    [](const HttpRequest&, UploadStreamContext& ctx, HttpResponse& resp) {
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
