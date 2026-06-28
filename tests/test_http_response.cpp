#include "http_response.h"

#include <gtest/gtest.h>

using namespace hps;

TEST(HttpResponseTest, DefaultState) {
  HttpResponse res;
  EXPECT_EQ(res.version, "HTTP/1.1");
  EXPECT_EQ(res.status_code, 200);
  EXPECT_EQ(res.status_text, "OK");
  EXPECT_TRUE(res.headers.empty());
  EXPECT_TRUE(res.body.empty());
}

TEST(HttpResponseTest, SetStatus) {
  HttpResponse res;
  res.set_status(404, "Not Found");
  EXPECT_EQ(res.status_code, 404);
  EXPECT_EQ(res.status_text, "Not Found");
}

TEST(HttpResponseTest, SetHeader) {
  HttpResponse res;
  res.set_header("Content-Type", "text/plain");
  EXPECT_EQ(res.headers["Content-Type"], "text/plain");
}

TEST(HttpResponseTest, SetContentType) {
  HttpResponse res;
  res.set_content_type("application/json");
  EXPECT_EQ(res.headers["Content-Type"], "application/json");
}

TEST(HttpResponseTest, SerializeBasic) {
  HttpResponse res;
  res.set_status(200, "OK");
  res.set_content_type("text/plain");
  res.set_content_length(5);
  res.body = "hello";
  std::string raw = res.serialize();
  EXPECT_NE(raw.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(raw.find("Content-Type: text/plain\r\n"), std::string::npos);
  EXPECT_NE(raw.find("Content-Length: 5\r\n"), std::string::npos);
  EXPECT_NE(raw.find("\r\n\r\nhello"), std::string::npos);
}

TEST(HttpResponseTest, SerializeNoBody) {
  HttpResponse res;
  res.set_status(204, "No Content");
  std::string raw = res.serialize();
  EXPECT_EQ(raw.back(), '\n');
  EXPECT_NE(raw.find("HTTP/1.1 204 No Content\r\n"), std::string::npos);
}

TEST(HttpResponseTest, Clear) {
  HttpResponse res;
  res.set_status(500, "Internal Server Error");
  res.body = "error";
  res.clear();
  EXPECT_EQ(res.status_code, 200);
  EXPECT_EQ(res.status_text, "OK");
  EXPECT_TRUE(res.body.empty());
  EXPECT_TRUE(res.headers.empty());
}

TEST(HttpResponseTest, HeaderCaseInsensitivity) {
  HttpResponse res;
  res.set_header("CONTENT-TYPE", "text/html");
  EXPECT_EQ(res.headers["content-type"], "text/html");
  EXPECT_EQ(res.headers["Content-Type"], "text/html");
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
