#include "ctcpclient.h"
#include "http_server.h"
#include "thread_pool.h"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

using namespace hps;

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace {

// 辅助：发送原始 HTTP 请求并读取响应
std::string send_raw(uint16_t port, const std::string& raw) {
  CTcpClient client("127.0.0.1", port);
  if (!client.connectToServer()) {
    return "";
  }
  if (!client.sendMessage(raw)) {
    return "";
  }
  std::string resp;
  client.receiveMessage(resp, ReadMode::Raw, 2000);
  return resp;
}

} // namespace

// TH1: GET 基本请求
TEST(HttpServerTest, GetBasic) {
  HttpServer server(CTcpServer::Config{0, 128, 2, 50});
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
  HttpServer server(CTcpServer::Config{0, 128, 2, 50});
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
  HttpServer server(CTcpServer::Config{0, 128, 2, 50});
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
  HttpServer server(CTcpServer::Config{0, 128, 2, 50});
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

  auto resp = send_raw(port,
    "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp.find("200 OK"), std::string::npos) << "响应: " << resp;
  ASSERT_NE(resp.find("hello"), std::string::npos) << "响应: " << resp;
}

// TH5: Keep-Alive 同连接多请求
TEST(HttpServerTest, KeepAlive) {
  HttpServer server(CTcpServer::Config{0, 128, 2, 50});
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
  CTcpClient client("127.0.0.1", port);
  ASSERT_TRUE(client.connectToServer());
  ASSERT_TRUE(client.sendMessage("GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n"));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  std::string resp1;
  client.receiveMessage(resp1, ReadMode::Raw, 2000);
  ASSERT_NE(resp1.find("pong"), std::string::npos) << "第一次响应: " << resp1;

  ASSERT_TRUE(client.sendMessage("GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  std::string resp2;
  client.receiveMessage(resp2, ReadMode::Raw, 2000);

  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp2.find("pong"), std::string::npos) << "第二次响应: " << resp2;
}

// TH6: 畸形请求 400
TEST(HttpServerTest, Malformed) {
  HttpServer server(CTcpServer::Config{0, 128, 2, 50});
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

// TH7: handler 异常返回 500
TEST(HttpServerTest, HandlerException) {
  HttpServer server(CTcpServer::Config{0, 128, 2, 50});
  server.get("/boom", [](const HttpRequest&, HttpResponse&) {
    throw std::runtime_error("boom!");
  });

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
