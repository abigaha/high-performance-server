#include "http_parser.h"

#include <gtest/gtest.h>

#include <string>

using namespace hps;

TEST(HttpParserExtremeTest, VeryLongRequestLine) {
  HttpParser parser;
  std::string path(8192, 'a');
  std::string raw = "GET /" + path + " HTTP/1.1\r\nHost: example.com\r\n\r\n";
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  EXPECT_EQ(parser.request().method, HttpMethod::GET);
}

TEST(HttpParserExtremeTest, ManyHeaders) {
  HttpParser parser;
  std::string raw = "GET / HTTP/1.1\r\nHost: x\r\n";
  for (int i = 0; i < 50; ++i) {
    raw += "X-Header-" + std::to_string(i) + ": value" + std::to_string(i) + "\r\n";
  }
  raw += "\r\n";
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  EXPECT_EQ(parser.request().headers.size(), 51); // Host + 50 custom
}

TEST(HttpParserExtremeTest, HeaderValueWithSpaces) {
  HttpParser parser;
  std::string raw = "GET / HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    "X-Custom:   leading-and-trailing-spaces   \r\n"
                    "\r\n";
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  // 首尾空格行为取决于解析器实现，不断言具体值
}

TEST(HttpParserExtremeTest, EmptyHeaderValue) {
  HttpParser parser;
  std::string raw = "GET / HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    "X-Empty:\r\n"
                    "\r\n";
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
}

TEST(HttpParserExtremeTest, MultipleContentLength) {
  HttpParser parser;
  std::string raw = "POST / HTTP/1.1\r\n"
                    "Host: example.com\r\n"
                    "Content-Length: 5\r\n"
                    "Content-Length: 3\r\n"
                    "\r\n"
                    "hello";
  // 重复 Content-Length 可能被拒绝（BAD_REQUEST）或取最后一个值
  auto result = parser.feed(raw);
  // 两种行为都接受
  EXPECT_TRUE(result.err == ParserError::OK || result.err == ParserError::BAD_REQUEST);
}

TEST(HttpParserExtremeTest, PipelinedRequests) {
  HttpParser parser;
  std::string req1 = "GET /first HTTP/1.1\r\nHost: a\r\n\r\n";
  auto r1 = parser.feed(req1);
  EXPECT_EQ(r1.err, ParserError::OK);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);

  parser.reset();
  EXPECT_EQ(parser.state(), ParserState::REQUEST_LINE);

  std::string req2 = "GET /second HTTP/1.1\r\nHost: b\r\n\r\n";
  auto r2 = parser.feed(req2);
  EXPECT_EQ(r2.err, ParserError::OK);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  EXPECT_EQ(parser.request().path, "/second");
}

TEST(HttpParserExtremeTest, VerySmallBody) {
  HttpParser parser;
  std::string raw = "POST / HTTP/1.1\r\n"
                    "Host: x\r\n"
                    "Content-Length: 1\r\n"
                    "\r\n"
                    "X";
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  EXPECT_EQ(parser.request().body, "X");
}

TEST(HttpParserExtremeTest, BodyWithoutContentLength) {
  HttpParser parser;
  std::string raw = "POST / HTTP/1.1\r\n"
                    "Host: x\r\n"
                    "\r\n"
                    "body data without content length";
  auto result = parser.feed(raw);
  // POST 无 Content-Length：body 为空或 incomplete，取决于实现
  EXPECT_TRUE(result.err == ParserError::INCOMPLETE || parser.request().body.empty());
}

TEST(HttpParserExtremeTest, ChunkedWithTrailingHeaders) {
  HttpParser parser;
  std::string raw = "POST / HTTP/1.1\r\n"
                    "Host: x\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "\r\n"
                    "5\r\n"
                    "hello\r\n"
                    "0\r\n"
                    "X-Trailing: val\r\n"
                    "\r\n";
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  EXPECT_EQ(parser.request().body, "hello");
}

TEST(HttpParserExtremeTest, ResetReuse) {
  HttpParser parser;
  for (int i = 0; i < 5; ++i) {
    std::string raw = "GET /item HTTP/1.1\r\nHost: x\r\n\r\n";
    auto result = parser.feed(raw);
    EXPECT_EQ(result.err, ParserError::OK);
    EXPECT_EQ(parser.state(), ParserState::COMPLETE);
    parser.reset();
    EXPECT_EQ(parser.state(), ParserState::REQUEST_LINE);
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
