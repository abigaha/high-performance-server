#include "http_response.h"
#include "range_parser.h"

#include <gtest/gtest.h>

#include <string>

using namespace hps;

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace {

// T1: 单范围 bytes=0-499
TEST(RangeParserTest, SingleRange) {
  auto r = parse_range_header("bytes=0-499", 1000);
  ASSERT_TRUE(r.valid);
  ASSERT_TRUE(r.satisfiable);
  ASSERT_EQ(r.ranges.size(), 1);
  EXPECT_EQ(r.ranges[0].start, 0);
  EXPECT_EQ(r.ranges[0].end, 500);
}

// T2: 开放范围 bytes=500-
TEST(RangeParserTest, OpenRange) {
  auto r = parse_range_header("bytes=500-", 1000);
  ASSERT_TRUE(r.valid);
  ASSERT_TRUE(r.satisfiable);
  ASSERT_EQ(r.ranges.size(), 1);
  EXPECT_EQ(r.ranges[0].start, 500);
  EXPECT_EQ(r.ranges[0].end, 1000);
}

// T3: 后缀范围 bytes=-500
TEST(RangeParserTest, SuffixRange) {
  auto r = parse_range_header("bytes=-500", 1000);
  ASSERT_TRUE(r.valid);
  ASSERT_TRUE(r.satisfiable);
  ASSERT_EQ(r.ranges.size(), 1);
  EXPECT_EQ(r.ranges[0].start, 500);
  EXPECT_EQ(r.ranges[0].end, 1000);
}

// T4: 多范围 bytes=0-99,200-299
TEST(RangeParserTest, MultiRange) {
  auto r = parse_range_header("bytes=0-99,200-299", 1000);
  ASSERT_TRUE(r.valid);
  ASSERT_TRUE(r.satisfiable);
  ASSERT_EQ(r.ranges.size(), 2);
  EXPECT_EQ(r.ranges[0].start, 0);
  EXPECT_EQ(r.ranges[0].end, 100);
  EXPECT_EQ(r.ranges[1].start, 200);
  EXPECT_EQ(r.ranges[1].end, 300);
}

// T5: 非法 Range
TEST(RangeParserTest, Invalid) {
  EXPECT_FALSE(parse_range_header("", 1000).valid);
  EXPECT_FALSE(parse_range_header("bytes=", 1000).valid);
  EXPECT_FALSE(parse_range_header("foobar", 1000).valid);
  EXPECT_FALSE(parse_range_header("bytes=abc", 1000).valid);
  EXPECT_FALSE(parse_range_header("bytes=0-abc", 1000).valid);
  EXPECT_FALSE(parse_range_header("bytes=5-3", 1000).valid);
}

// T6: 超出文件大小 → satisfiable=false
TEST(RangeParserTest, Unsatisfiable) {
  auto r = parse_range_header("bytes=9000-9999", 1000);
  ASSERT_TRUE(r.valid);
  ASSERT_FALSE(r.satisfiable);
}

// T7: 206 headers 单范围
TEST(RangeParserTest, PartialResponseHeaders) {
  auto range = parse_range_header("bytes=100-199", 1000);
  ASSERT_TRUE(range.valid);
  ASSERT_TRUE(range.satisfiable);

  HttpResponse resp;
  build_206_headers(resp, range, 1000);

  EXPECT_EQ(resp.status_code, 206);
  EXPECT_EQ(resp.status_text, "Partial Content");
  auto it = resp.headers.find("Content-Range");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "bytes 100-199/1000");
  EXPECT_EQ(resp.body.size(), 0);
}

// T8: 206 响应后缀范围
TEST(RangeParserTest, PartialResponseSuffix) {
  auto range = parse_range_header("bytes=-200", 1000);
  ASSERT_TRUE(range.valid);
  ASSERT_TRUE(range.satisfiable);

  HttpResponse resp;
  build_206_headers(resp, range, 1000);

  auto it = resp.headers.find("Content-Range");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "bytes 800-999/1000");
}

// T9: 416 响应
TEST(RangeParserTest, UnsatisfiableResponse) {
  HttpResponse resp;
  build_416_response(resp, 1000);

  EXPECT_EQ(resp.status_code, 416);
  EXPECT_EQ(resp.status_text, "Range Not Satisfiable");
  auto it = resp.headers.find("Content-Range");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "bytes */1000");
}

// T10: 生成 boundary 非空
TEST(RangeParserTest, GenerateBoundary) {
  auto b1 = generate_boundary();
  auto b2 = generate_boundary();
  EXPECT_FALSE(b1.empty());
  EXPECT_TRUE(b1.find("HPS_") == 0);
  EXPECT_NE(b1, b2);
}

} // namespace
