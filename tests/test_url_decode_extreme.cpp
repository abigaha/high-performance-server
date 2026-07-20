#include "url_decode.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

using namespace hps;

TEST(UrlDecodeExtremeTest, SimpleDecode) {
  std::string out;
  EXPECT_TRUE(url_decode("hello%20world", out));
  EXPECT_EQ(out, "hello world");
}

TEST(UrlDecodeExtremeTest, PlusAsSpace) {
  std::string out;
  EXPECT_TRUE(url_decode("a+b", out));
  EXPECT_EQ(out, "a b");
}

TEST(UrlDecodeExtremeTest, MixedPercentPlus) {
  std::string out;
  EXPECT_TRUE(url_decode("%21%40%23+test", out));
  EXPECT_EQ(out, "!@# test");
}

TEST(UrlDecodeExtremeTest, InvalidPercentEncoding) {
  std::string out;
  EXPECT_FALSE(url_decode("%XY", out));
  EXPECT_TRUE(out.empty());
}

TEST(UrlDecodeExtremeTest, AllPrintableChars) {
  std::string encoded;
  std::string expected;
  for (int c = 32; c <= 126; ++c) {
    expected += static_cast<char>(c);
    if (c == ' ') {
      encoded += '+';
    } else if (c == '+' || c == '%' || c == '#' || c == '&' || c == '=' || c == '?' || c == '/') {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      encoded += hex;
    } else {
      encoded += static_cast<char>(c);
    }
  }

  std::string out;
  EXPECT_TRUE(url_decode(encoded, out));
  EXPECT_EQ(out, expected);
}

TEST(UrlDecodeExtremeTest, EmptyInput) {
  std::string out;
  EXPECT_TRUE(url_decode("", out));
  EXPECT_TRUE(out.empty());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
