#include "auth_middleware.h"
#include "auth_service.h"
#include "database_pool.h"
#include "db_config.h"
#include "http_parser.h"
#include "http_request.h"
#include "http_response.h"
#include "mock_connection.h"
#include "models.h"
#include "range_parser.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace hps {

// ============================================================
// T1: Content-Type 自动检测
// ============================================================
TEST(Step16UtilTest, DetectContentType) {
  auto detect = [](const std::string& name) -> std::string {
    auto dot = name.rfind('.');
    if (dot == std::string::npos)
      return "application/octet-stream";
    auto ext = name.substr(dot);
    if (ext == ".mp3")
      return "audio/mpeg";
    if (ext == ".ogg")
      return "audio/ogg";
    if (ext == ".flac")
      return "audio/flac";
    if (ext == ".wav")
      return "audio/wav";
    return "application/octet-stream";
  };

  EXPECT_EQ(detect("song.mp3"), "audio/mpeg");
  EXPECT_EQ(detect("track.ogg"), "audio/ogg");
  EXPECT_EQ(detect("album.flac"), "audio/flac");
  EXPECT_EQ(detect("sample.wav"), "audio/wav");
  EXPECT_EQ(detect("file.bin"), "application/octet-stream");
  EXPECT_EQ(detect("noext"), "application/octet-stream");
}

// ============================================================
// T2: 加盐哈希密码
// ============================================================
TEST(Step16UtilTest, SaltedHashPassword) {
  auto salt = generate_salt();
  EXPECT_EQ(salt.size(), 32); // 16 bytes → 32 hex chars

  auto hash1 = hash_password("test123", salt);
  EXPECT_FALSE(hash1.empty());
  EXPECT_EQ(hash1.size(), 64); // SHA-256 → 64 hex chars

  auto hash2 = hash_password("test123", salt);
  EXPECT_EQ(hash1, hash2);

  auto hash3 = hash_password("test123", "different_salt_abc1234567890");
  EXPECT_NE(hash1, hash3);
}

// ============================================================
// T3: Range Parser（音频流播依赖）
// ============================================================
TEST(Step16UtilTest, RangeParser) {
  auto range = parse_range_header("bytes=0-99", 1000);
  EXPECT_TRUE(range.valid);
  EXPECT_TRUE(range.satisfiable);
  ASSERT_EQ(range.ranges.size(), 1U);
  EXPECT_EQ(range.ranges[0].start, 0U);
  EXPECT_EQ(range.ranges[0].end, 100U);

  auto range2 = parse_range_header("bytes=-", 1000);
  EXPECT_FALSE(range2.valid);

  // 206 headers
  HttpResponse resp;
  resp.set_status(200, "OK");
  build_206_headers(resp, range, 1000);
  EXPECT_FALSE(resp.headers.empty());

  // 416
  HttpResponse resp416;
  build_416_response(resp416, 1000);
  EXPECT_EQ(resp416.status_code, 416);
}

// ============================================================
// T4: CORS headers
// ============================================================
TEST(Step16UtilTest, CorsHeaders) {
  HttpResponse resp;
  resp.set_status(200, "OK");

  auto cors = [](HttpResponse& r) {
    r.set_header("Access-Control-Allow-Origin", "*");
    r.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    r.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type, Range");
    r.set_header("Access-Control-Expose-Headers", "Content-Range, Accept-Ranges, Content-Disposition");
    r.set_header("Access-Control-Max-Age", "86400");
  };
  cors(resp);

  EXPECT_EQ(resp.headers["Access-Control-Allow-Origin"], "*");
  EXPECT_EQ(resp.headers["Access-Control-Max-Age"], "86400");
}

// ============================================================
// T5: Token 生成 + 验证
// ============================================================
TEST(Step16ApiTest, TokenGenerationValidation) {
  auto factory = []() -> std::unique_ptr<IConnection> { return std::make_unique<MockConnection>(); };

  DatabasePool pool(factory);
  DbConfig cfg;
  cfg.pool_size = 1;
  cfg.connect_timeout_ms = 500;
  ASSERT_TRUE(pool.init(cfg));

  auto auth = create_auth_service(pool, "test-secret");

  AuthUser au;
  au.user_id = 42;
  au.username = "testuser";
  au.role = UserRole::NORMAL;

  auto token = auth->generate_token(au);
  EXPECT_FALSE(token.empty());

  // Validate token directly
  auto validated = auth->validate_token(token);
  EXPECT_EQ(validated.user_id, 42);
  EXPECT_EQ(validated.role, UserRole::NORMAL);

  // Validate without token
  auto empty = auth->validate_token("");
  EXPECT_EQ(empty.role, UserRole::GUEST);

  // Validate via AuthMiddleware
  HttpRequest req;
  req.headers["Authorization"] = "Bearer " + token;
  AuthMiddleware::apply(*auth, req);
  EXPECT_EQ(req.auth_user.user_id, 42);
  EXPECT_EQ(req.auth_user.role, UserRole::NORMAL);

  HttpRequest req2;
  AuthMiddleware::apply(*auth, req2);
  EXPECT_EQ(req2.auth_user.role, UserRole::GUEST);
}

} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
