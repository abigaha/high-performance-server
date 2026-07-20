#include "http_server.h"
#include "tcp_client.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace hps;

namespace {

std::string send_raw(uint16_t port, const std::string& raw) {
  TcpClient client("127.0.0.1", port);
  if (!client.connect_to_server()) {
    return "";
  }
  if (!client.send_message(raw)) {
    return "";
  }
  std::string resp;
  client.receive_message(resp, ReadMode::RAW, 3000);
  return resp;
}

} // namespace

TEST(HttpServerStressTest, BasicGetRequest) {
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.get("/", [](const HttpRequest&, HttpResponse& resp) {
    resp.set_status(200, "OK");
    resp.set_content_type("text/plain");
    resp.body = "hello world";
    resp.set_content_length(resp.body.size());
  });

  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  auto resp = send_raw(port, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp.find("200 OK"), std::string::npos) << "响应: " << resp;
  ASSERT_NE(resp.find("hello world"), std::string::npos) << "响应: " << resp;
}

TEST(HttpServerStressTest, PostWithBody) {
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
    send_raw(port,
             "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\nConnection: close\r\n\r\nhello world");
  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  ASSERT_NE(resp.find("200 OK"), std::string::npos) << "响应: " << resp;
  ASSERT_NE(resp.find("hello world"), std::string::npos) << "响应: " << resp;
}

TEST(HttpServerStressTest, ConcurrentConnections) {
  HttpServer server(TcpServer::Config{0, 128, 4, 50});
  server.get("/", [](const HttpRequest&, HttpResponse& resp) {
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

  std::atomic<int> success_count{0};
  std::vector<std::thread> clients;
  for (int i = 0; i < 4; ++i) {
    clients.emplace_back([port, &success_count]() {
      TcpClient client("127.0.0.1", port);
      if (client.connect_to_server()) {
        if (client.send_message("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")) {
          std::string resp;
          if (client.receive_message(resp, ReadMode::RAW, 3000)) {
            if (resp.find("200 OK") != std::string::npos) {
              success_count.fetch_add(1);
            }
          }
        }
      }
    });
  }
  for (auto& th : clients) {
    th.join();
  }

  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(success_count.load(), 4);
}

TEST(HttpServerStressTest, KeepAliveSequence) {
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

  TcpClient client("127.0.0.1", port);
  ASSERT_TRUE(client.connect_to_server());

  for (int i = 0; i < 3; ++i) {
    std::string req = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_TRUE(client.send_message(req));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::string resp;
    ASSERT_TRUE(client.receive_message(resp, ReadMode::RAW, 500));
    ASSERT_NE(resp.find("pong"), std::string::npos) << "第 " << i << " 次响应: " << resp;
  }

  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST(HttpServerStressTest, ConnectionClose) {
  HttpServer server(TcpServer::Config{0, 128, 2, 50});
  server.get("/close", [](const HttpRequest&, HttpResponse& resp) {
    resp.set_status(200, "OK");
    resp.set_header("Connection", "close");
    resp.body = "bye";
    resp.set_content_length(resp.body.size());
  });

  ASSERT_TRUE(server.init());
  std::thread t([&server]() { server.start(); });
  t.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint16_t port = server.actual_port();

  TcpClient client("127.0.0.1", port);
  ASSERT_TRUE(client.connect_to_server());
  ASSERT_TRUE(client.send_message("GET /close HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));

  std::string resp;
  client.receive_message(resp, ReadMode::RAW, 2000);
  ASSERT_NE(resp.find("200 OK"), std::string::npos) << "响应: " << resp;

  // 连接应已关闭，尝试接收额外数据应失败或返回空
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::string extra;
  bool received = client.receive_message(extra, ReadMode::RAW, 500);

  server.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_FALSE(received) << "连接应已关闭，但收到了额外数据: " << extra;
}

TEST(HttpServerStressTest, ServerStartStop) {
  // 第一次启动
  {
    HttpServer server(TcpServer::Config{0, 128, 2, 50});
    server.get("/", [](const HttpRequest&, HttpResponse& resp) {
      resp.set_status(200, "OK");
      resp.body = "ok";
      resp.set_content_length(resp.body.size());
    });
    ASSERT_TRUE(server.init());
    std::thread t([&server]() { server.start(); });
    t.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto port = server.actual_port();
    auto resp = send_raw(port, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_NE(resp.find("200 OK"), std::string::npos);
  }

  // 第二次启动（新实例）
  {
    HttpServer server(TcpServer::Config{0, 128, 2, 50});
    server.get("/", [](const HttpRequest&, HttpResponse& resp) {
      resp.set_status(200, "OK");
      resp.body = "ok";
      resp.set_content_length(resp.body.size());
    });
    ASSERT_TRUE(server.init());
    std::thread t([&server]() { server.start(); });
    t.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto port = server.actual_port();
    auto resp = send_raw(port, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_NE(resp.find("200 OK"), std::string::npos);
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
