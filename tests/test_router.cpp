#include "router.h"

#include <gtest/gtest.h>

#include <string>

using namespace hps;

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace {
Router::Handler make_handler(std::string tag) {
  return [tag = std::move(tag)](const HttpRequest&, HttpResponse&) {
    // 仅用于匹配验证，不实际处理
    (void)tag;
  };
}
} // namespace

// TR1: 静态路径匹配
TEST(RouterTest, StaticMatch) {
  Router r;
  r.add(HttpMethod::GET, "/api/health", make_handler("health"));
  Router::Handler h;
  std::unordered_map<std::string, std::string> params;
  ASSERT_TRUE(r.match(HttpMethod::GET, "/api/health", h, params));
  ASSERT_TRUE(params.empty());
}

// TR2: 单参数路径匹配
TEST(RouterTest, ParamMatch) {
  Router r;
  r.add(HttpMethod::GET, "/song/:id", make_handler("song"));
  Router::Handler h;
  std::unordered_map<std::string, std::string> params;
  ASSERT_TRUE(r.match(HttpMethod::GET, "/song/42", h, params));
  ASSERT_EQ(params.size(), 1U);
  EXPECT_EQ(params["id"], "42");
}

// TR3: 多参数路径匹配
TEST(RouterTest, MultiParam) {
  Router r;
  r.add(HttpMethod::GET, "/user/:uid/song/:sid", make_handler("us"));
  Router::Handler h;
  std::unordered_map<std::string, std::string> params;
  ASSERT_TRUE(r.match(HttpMethod::GET, "/user/1/song/2", h, params));
  ASSERT_EQ(params.size(), 2U);
  EXPECT_EQ(params["uid"], "1");
  EXPECT_EQ(params["sid"], "2");
}

// TR4: 未注册路径不匹配
TEST(RouterTest, NoMatchPath) {
  Router r;
  r.add(HttpMethod::GET, "/api/health", make_handler("health"));
  Router::Handler h;
  std::unordered_map<std::string, std::string> params;
  EXPECT_FALSE(r.match(HttpMethod::GET, "/api/unknown", h, params));
}

// TR5: 方法不符不匹配
TEST(RouterTest, MethodMismatch) {
  Router r;
  r.add(HttpMethod::GET, "/api/health", make_handler("health"));
  Router::Handler h;
  std::unordered_map<std::string, std::string> params;
  EXPECT_FALSE(r.match(HttpMethod::POST, "/api/health", h, params));
}

// TR6: 静态段优先于参数段
TEST(RouterTest, StaticBeforeParam) {
  Router r;
  r.add(HttpMethod::GET, "/song/list", make_handler("list"));
  r.add(HttpMethod::GET, "/song/:id", make_handler("id"));

  Router::Handler h;
  std::unordered_map<std::string, std::string> params;

  // /song/list 应匹配静态 handler
  ASSERT_TRUE(r.match(HttpMethod::GET, "/song/list", h, params));
  ASSERT_TRUE(params.empty());

  // /song/42 应匹配参数 handler
  ASSERT_TRUE(r.match(HttpMethod::GET, "/song/42", h, params));
  ASSERT_EQ(params.size(), 1U);
  EXPECT_EQ(params["id"], "42");
}

// TR7: 根路径匹配
TEST(RouterTest, RootPath) {
  Router r;
  r.add(HttpMethod::GET, "/", make_handler("root"));
  Router::Handler h;
  std::unordered_map<std::string, std::string> params;
  ASSERT_TRUE(r.match(HttpMethod::GET, "/", h, params));
  ASSERT_TRUE(params.empty());
}

// TR8: 尾斜杠归一化（/api/health 与 /api/health/ 视为同一路由）
TEST(RouterTest, TrailingSlash) {
  Router r;
  r.add(HttpMethod::GET, "/api/health", make_handler("health"));
  Router::Handler h;
  std::unordered_map<std::string, std::string> params;
  // 注册无尾斜杠，请求带尾斜杠，应匹配
  EXPECT_TRUE(r.match(HttpMethod::GET, "/api/health/", h, params));
}

// TR9: 同一路径不同方法各自注册
TEST(RouterTest, SamePathDiffMethod) {
  Router r;
  r.add(HttpMethod::GET, "/song/:id", make_handler("get"));
  r.add(HttpMethod::DELETE, "/song/:id", make_handler("del"));

  Router::Handler h;
  std::unordered_map<std::string, std::string> params;
  ASSERT_TRUE(r.match(HttpMethod::GET, "/song/1", h, params));
  ASSERT_TRUE(r.match(HttpMethod::DELETE, "/song/1", h, params));
  EXPECT_FALSE(r.match(HttpMethod::POST, "/song/1", h, params));
}

// TR10: 文件 ID 下载路由与按哈希下载路由不冲突
TEST(RouterTest, FileDownloadRoutesDoNotConflict) {
  Router r;
  std::string matched_route;
  r.add(HttpMethod::GET, "/api/files/:id/download", [&matched_route](const HttpRequest&, HttpResponse&) {
    matched_route = "id";
  });
  r.add(HttpMethod::GET, "/api/files/by-hash/:hash/download", [&matched_route](const HttpRequest&, HttpResponse&) {
    matched_route = "hash";
  });

  Router::Handler h;
  std::unordered_map<std::string, std::string> params;
  HttpRequest request;
  HttpResponse response;

  ASSERT_TRUE(r.match(HttpMethod::GET, "/api/files/42/download", h, params));
  ASSERT_EQ(params.size(), 1U);
  EXPECT_EQ(params.at("id"), "42");
  EXPECT_EQ(params.count("hash"), 0U);
  h(request, response);
  EXPECT_EQ(matched_route, "id");

  params.clear();
  matched_route.clear();
  ASSERT_TRUE(r.match(HttpMethod::GET, "/api/files/by-hash/3a7bd3e2360a/download", h, params));
  ASSERT_EQ(params.size(), 1U);
  EXPECT_EQ(params.count("id"), 0U);
  EXPECT_EQ(params.at("hash"), "3a7bd3e2360a");
  h(request, response);
  EXPECT_EQ(matched_route, "hash");
}
