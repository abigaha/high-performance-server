#include "detail/mysql_field_to_string.h"

#include <gtest/gtest.h>

#include <array>
#include <boost/mysql/blob_view.hpp>
#include <boost/mysql/date.hpp>
#include <boost/mysql/datetime.hpp>
#include <boost/mysql/field_view.hpp>
#include <boost/mysql/string_view.hpp>
#include <boost/mysql/time.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

namespace hps {
namespace {

TEST(MySqlFieldToStringTest, ConvertsSupportedFields) {
  const std::string text_with_null{"test\0value", 10};
  const boost::mysql::field_view null_field;
  const boost::mysql::field_view signed_field{std::numeric_limits<std::int64_t>::min()};
  const boost::mysql::field_view unsigned_field{std::numeric_limits<std::uint64_t>::max()};
  const boost::mysql::field_view text_field{boost::mysql::string_view{text_with_null}};
  const std::array<unsigned char, 3> blob{0x00U, 0x41U, 0xffU};
  const std::string expected_blob{'\0', 'A', static_cast<char>(0xffU)};
  const boost::mysql::field_view blob_field{boost::mysql::blob_view{blob}};
  const boost::mysql::field_view float_field{1.5F};
  const boost::mysql::field_view double_field{-0.125};
  const auto time = -std::chrono::duration_cast<boost::mysql::time>(
    std::chrono::hours{123} + std::chrono::minutes{45} + std::chrono::seconds{6} + std::chrono::microseconds{7});
  const boost::mysql::field_view date_field{boost::mysql::date{2026, 7, 22}};
  const boost::mysql::field_view datetime_field{boost::mysql::datetime{2026, 7, 22, 14, 33, 21, 42}};
  const boost::mysql::field_view time_field{time};

  EXPECT_EQ(detail::mysql_field_to_string(null_field), "");
  EXPECT_EQ(detail::mysql_field_to_string(signed_field), "-9223372036854775808");
  EXPECT_EQ(detail::mysql_field_to_string(unsigned_field), "18446744073709551615");
  EXPECT_EQ(detail::mysql_field_to_string(text_field), text_with_null);
  EXPECT_EQ(detail::mysql_field_to_string(blob_field), expected_blob);
  EXPECT_EQ(detail::mysql_field_to_string(float_field), "1.5");
  EXPECT_EQ(detail::mysql_field_to_string(double_field), "-0.125");
  EXPECT_EQ(detail::mysql_field_to_string(date_field), "2026-07-22");
  EXPECT_EQ(detail::mysql_field_to_string(datetime_field), "2026-07-22 14:33:21.000042");
  EXPECT_EQ(detail::mysql_field_to_string(time_field), "-123:45:06.000007");
}

} // namespace
} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
