#include "http_parser.h"

#include <gtest/gtest.h>

#include <string>

using namespace hps;

TEST(HttpParserTest, SimpleGet) {
  HttpParser parser;
  std::string raw = "GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n";
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(result.consumed, raw.size());
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  EXPECT_EQ(parser.request().method, HttpMethod::GET);
  EXPECT_EQ(parser.request().path, "/index.html");
  EXPECT_EQ(parser.request().version, "HTTP/1.1");
  EXPECT_EQ(parser.request().headers["Host"], "example.com");
}

TEST(HttpParserTest, PostWithBody) {
  HttpParser parser;
  std::string raw = "POST /submit HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(result.consumed, raw.size());
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  EXPECT_EQ(parser.request().method, HttpMethod::POST);
  EXPECT_EQ(parser.request().path, "/submit");
  EXPECT_EQ(parser.request().body, "hello");
}

TEST(HttpParserTest, PartialFeed) {
  HttpParser parser;
  auto result = parser.feed("GET / HTTP/1.1\r\n");
  EXPECT_EQ(result.err, ParserError::INCOMPLETE);
  EXPECT_EQ(parser.state(), ParserState::HEADERS);

  result = parser.feed("Host: a\r\n\r\n");
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(result.consumed, 11);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  EXPECT_EQ(parser.request().method, HttpMethod::GET);
  EXPECT_EQ(parser.request().path, "/");
  EXPECT_EQ(parser.request().headers["Host"], "a");
}

TEST(HttpParserTest, QueryString) {
  HttpParser parser;
  std::string raw = "GET /search?q=test&n=1 HTTP/1.1\r\n\r\n";
  parser.feed(raw);
  EXPECT_EQ(parser.request().path, "/search");
  EXPECT_EQ(parser.request().query_string, "q=test&n=1");
}

TEST(HttpParserTest, PayloadTooLarge) {
  HttpParser parser;
  std::string body(200 * 1024 * 1024, 'x');
  std::string raw = "POST / HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::PAYLOAD_TOO_LARGE);
}

TEST(HttpParserTest, BadRequestLine) {
  HttpParser parser;
  auto result = parser.feed("INVALID\r\n\r\n");
  EXPECT_EQ(result.err, ParserError::BAD_REQUEST);
}

TEST(HttpParserTest, BadHeaderMissingColon) {
  HttpParser parser;
  auto result = parser.feed("GET / HTTP/1.1\r\nBadHeader\r\n\r\n");
  EXPECT_EQ(result.err, ParserError::BAD_REQUEST);
}

TEST(HttpParserTest, Reset) {
  HttpParser parser;
  parser.feed("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);

  parser.reset();
  EXPECT_EQ(parser.state(), ParserState::REQUEST_LINE);
  EXPECT_TRUE(parser.request().path.empty());
  EXPECT_TRUE(parser.request().headers.empty());
}

TEST(HttpParserTest, CaseInsensitiveHeaders) {
  HttpParser parser;
  std::string raw = "GET / HTTP/1.1\r\nCONTENT-TYPE: text/html\r\n\r\n";
  parser.feed(raw);
  EXPECT_EQ(parser.request().headers["content-type"], "text/html");
  EXPECT_EQ(parser.request().headers["Content-Type"], "text/html");
}

TEST(HttpParserTest, ChunkedTransferEncoding) {
  HttpParser parser;
  std::string raw = "POST / HTTP/1.1\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "\r\n"
                    "5\r\n"
                    "hello\r\n"
                    "0\r\n"
                    "\r\n";
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  EXPECT_EQ(parser.request().body, "hello");
}

TEST(HttpParserTest, ChunkedMultipleChunks) {
  HttpParser parser;
  std::string raw = "POST / HTTP/1.1\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "\r\n"
                    "3\r\n"
                    "hel\r\n"
                    "2\r\n"
                    "lo\r\n"
                    "0\r\n"
                    "\r\n";
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  EXPECT_EQ(parser.request().body, "hello");
}

TEST(HttpParserTest, ChunkedNoBody) {
  HttpParser parser;
  std::string raw = "POST / HTTP/1.1\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "\r\n"
                    "0\r\n"
                    "\r\n";
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  EXPECT_TRUE(parser.request().body.empty());
}

TEST(HttpParserTest, ParserErrorIncomplete) {
  HttpParser parser;
  auto result = parser.feed("GET / HTT");
  EXPECT_EQ(result.err, ParserError::INCOMPLETE);
}

TEST(HttpParserTest, ErrorStateNoMoreFeed) {
  HttpParser parser;
  parser.feed("BAD\r\n\r\n");
  auto result = parser.feed("GET / HTTP/1.1\r\n\r\n");
  EXPECT_EQ(result.err, ParserError::BAD_REQUEST);
  EXPECT_EQ(result.consumed, 0);
}

TEST(HttpParserTest, CompleteStateNoMoreFeed) {
  HttpParser parser;
  parser.feed("GET / HTTP/1.1\r\n\r\n");
  auto result = parser.feed("extra data");
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(result.consumed, 0);
}

TEST(HttpParserTest, HeaderTrimmed) {
  HttpParser parser;
  std::string raw = "GET / HTTP/1.1\r\nHost:   example.com  \r\nX-Custom:  val\r\n\r\n";
  parser.feed(raw);
  EXPECT_EQ(parser.request().headers["Host"], "example.com  ");
  EXPECT_EQ(parser.request().headers["X-Custom"], "val");
}

TEST(HttpParserTest, ChunkedWithExtensions) {
  HttpParser parser;
  std::string raw = "POST / HTTP/1.1\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "\r\n"
                    "5;ext=1\r\n"
                    "hello\r\n"
                    "0;ext=2\r\n"
                    "\r\n";
  auto result = parser.feed(raw);
  EXPECT_EQ(result.err, ParserError::OK);
  EXPECT_EQ(parser.state(), ParserState::COMPLETE);
  EXPECT_EQ(parser.request().body, "hello");
}

TEST(HttpParserTest, DeleteMethod) {
  HttpParser parser;
  std::string raw = "DELETE /resource/42 HTTP/1.1\r\n\r\n";
  parser.feed(raw);
  EXPECT_EQ(parser.request().method, HttpMethod::DELETE);
  EXPECT_EQ(parser.request().path, "/resource/42");
}

TEST(HttpParserTest, HeadersMultipleValues) {
  HttpParser parser;
  std::string raw = "GET / HTTP/1.1\r\nAccept: text/html\r\nAccept: application/json\r\n\r\n";
  parser.feed(raw);
  EXPECT_EQ(parser.request().headers["Accept"], "application/json");
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
