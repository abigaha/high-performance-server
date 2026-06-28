#include "url_decode.h"

#include <gtest/gtest.h>

using namespace hps;

TEST(UrlDecodeTest, PlainString) {
  std::string out;
  EXPECT_TRUE(url_decode("hello", out));
  EXPECT_EQ(out, "hello");
}

TEST(UrlDecodeTest, SpaceAsPlus) {
  std::string out;
  EXPECT_TRUE(url_decode("hello+world", out));
  EXPECT_EQ(out, "hello world");
}

TEST(UrlDecodeTest, PercentEncoded) {
  std::string out;
  EXPECT_TRUE(url_decode("hello%20world", out));
  EXPECT_EQ(out, "hello world");
}

TEST(UrlDecodeTest, Mixed) {
  std::string out;
  EXPECT_TRUE(url_decode("a%2Fb+c", out));
  EXPECT_EQ(out, "a/b c");
}

TEST(UrlDecodeTest, InvalidPercent) {
  std::string out;
  EXPECT_FALSE(url_decode("%XX", out));
}

TEST(UrlDecodeTest, TruncatedPercent) {
  std::string out;
  EXPECT_FALSE(url_decode("%", out));
  EXPECT_FALSE(url_decode("%A", out));
}

TEST(UrlDecodeTest, Empty) {
  std::string out;
  EXPECT_TRUE(url_decode("", out));
  EXPECT_TRUE(out.empty());
}

TEST(UrlDecodeTest, HexCaseInsensitive) {
  std::string out;
  EXPECT_TRUE(url_decode("%2f", out));
  EXPECT_EQ(out, "/");
  out.clear();
  EXPECT_TRUE(url_decode("%2F", out));
  EXPECT_EQ(out, "/");
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
