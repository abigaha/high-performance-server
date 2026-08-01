#include "http_request.h"

#include <gtest/gtest.h>

using namespace hps;

TEST(HttpRequestTest, DefaultState) {
  HttpRequest req;
  EXPECT_EQ(req.auth_status, TokenValidationStatus::INVALID);
  EXPECT_EQ(req.method, HttpMethod::UNKNOWN);
  EXPECT_TRUE(req.path.empty());
  EXPECT_TRUE(req.query_string.empty());
  EXPECT_EQ(req.version, "HTTP/1.1");
  EXPECT_TRUE(req.headers.empty());
  EXPECT_TRUE(req.body.empty());
}

TEST(HttpRequestTest, Clear) {
  HttpRequest req;
  req.method = HttpMethod::POST;
  req.path = "/test";
  req.headers["X-Custom"] = "val";
  req.body = "data";
  req.clear();
  EXPECT_EQ(req.method, HttpMethod::UNKNOWN);
  EXPECT_TRUE(req.path.empty());
  EXPECT_TRUE(req.headers.empty());
  EXPECT_TRUE(req.body.empty());
}

TEST(HttpRequestTest, ClearResetsEffectiveIdentity) {
  HttpRequest req;
  req.auth_user =
    EffectiveIdentity{42, "vip-user", UserRole::VIP, VipStatus::ACTIVE, std::chrono::system_clock::time_point::max()};

  req.clear();

  EXPECT_EQ(req.auth_user.user_id, 0);
  EXPECT_EQ(req.auth_user.role, UserRole::GUEST);
  EXPECT_EQ(req.auth_user.vip_status, VipStatus::NONE);
  EXPECT_FALSE(req.auth_user.vip_expires_at.has_value());
  EXPECT_EQ(req.auth_status, TokenValidationStatus::INVALID);
}

TEST(HttpMethodTest, ToString) {
  EXPECT_EQ(http_method_to_string(HttpMethod::GET), "GET");
  EXPECT_EQ(http_method_to_string(HttpMethod::POST), "POST");
  EXPECT_EQ(http_method_to_string(HttpMethod::UNKNOWN), "UNKNOWN");
}

TEST(HttpMethodTest, FromString) {
  EXPECT_EQ(string_to_http_method("GET"), HttpMethod::GET);
  EXPECT_EQ(string_to_http_method("POST"), HttpMethod::POST);
  EXPECT_EQ(string_to_http_method("DELETE"), HttpMethod::DELETE);
  EXPECT_EQ(string_to_http_method("PATCH"), HttpMethod::PATCH);
  EXPECT_EQ(string_to_http_method("INVALID"), HttpMethod::UNKNOWN);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
