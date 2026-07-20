#include "http_response.h"
#include "range_parser.h"

#include <gtest/gtest.h>

#include <string>

using namespace hps;

TEST(RangeParserExtremeTest, SingleRange) {
  auto r = parse_range_header("bytes=0-100", 1000);
  ASSERT_TRUE(r.valid);
  ASSERT_TRUE(r.satisfiable);
  ASSERT_EQ(r.ranges.size(), 1);
  EXPECT_EQ(r.ranges[0].start, 0);
  EXPECT_EQ(r.ranges[0].end, 101);
}

TEST(RangeParserExtremeTest, MultipleRanges) {
  auto r = parse_range_header("bytes=0-50,100-150", 1000);
  ASSERT_TRUE(r.valid);
  ASSERT_TRUE(r.satisfiable);
  ASSERT_EQ(r.ranges.size(), 2);
  EXPECT_EQ(r.ranges[0].start, 0);
  EXPECT_EQ(r.ranges[0].end, 51);
  EXPECT_EQ(r.ranges[1].start, 100);
  EXPECT_EQ(r.ranges[1].end, 151);
}

TEST(RangeParserExtremeTest, OpenEndedRange) {
  auto r = parse_range_header("bytes=100-", 500);
  ASSERT_TRUE(r.valid);
  ASSERT_TRUE(r.satisfiable);
  ASSERT_EQ(r.ranges.size(), 1);
  EXPECT_EQ(r.ranges[0].start, 100);
  EXPECT_EQ(r.ranges[0].end, 500);
}

TEST(RangeParserExtremeTest, PrefixRange) {
  auto r = parse_range_header("bytes=-100", 1000);
  ASSERT_TRUE(r.valid);
  ASSERT_TRUE(r.satisfiable);
  ASSERT_EQ(r.ranges.size(), 1);
  EXPECT_EQ(r.ranges[0].start, 900);
  EXPECT_EQ(r.ranges[0].end, 1000);
}

TEST(RangeParserExtremeTest, InvalidRangeFormat) {
  EXPECT_FALSE(parse_range_header("", 1000).valid);
  EXPECT_FALSE(parse_range_header("bytes=", 1000).valid);
  EXPECT_FALSE(parse_range_header("invalid", 1000).valid);
  EXPECT_FALSE(parse_range_header("bytes=abc", 1000).valid);
  EXPECT_FALSE(parse_range_header("bytes=0-abc", 1000).valid);
  EXPECT_FALSE(parse_range_header("bytes=5-3", 1000).valid);
}

TEST(RangeParserExtremeTest, RangeBeyondFileSize) {
  auto r = parse_range_header("bytes=9000-9999", 1000);
  ASSERT_TRUE(r.valid);
  ASSERT_FALSE(r.satisfiable);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
