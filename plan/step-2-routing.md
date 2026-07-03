# Step 2：路由注册 + 请求分发

> **状态**：✅ 已完成（commit `81c8270`）
> **起止**：基础设施完成后

## 背景

在已有 HTTP 解析器和 TCP Server 基础上，实现路由注册（前缀树 trie）和 HTTP 请求分发，支持静态段和参数段路由。

## 功能点

| # | 功能点 | 说明 |
|---|--------|------|
| F1 | **Router 前缀树** | trie 树路由，静态段+参数段（`:name`），静态优先+回溯 |
| F2 | **HttpServer 封装** | 封装 TcpServer+Router，per-conn 局部 parser，keep-alive |
| F3 | **HttpRequest 扩展** | 增加 `path_params`，支持路由参数提取 |
| F4 | **错误响应** | 400 Bad Request / 404 Not Found / 413 Payload Too Large / 500 Internal Server Error |
| F5 | **handler 异常捕获** | handler 异常不导致线程退出，返回 500 |

## 接口设计

### Router

```cpp
class Router : public IRouter {
public:
  using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;
  using Params = std::unordered_map<std::string, std::string>;

  void add(HttpMethod method, std::string_view path, Handler handler);
  bool match(HttpMethod method, std::string_view path,
             Handler& outHandler, Params& outParams) const;
};
```

### HttpServer

```cpp
class HttpServer : public IHttpServer {
public:
  using Handler = IRouter::Handler;
  explicit HttpServer(const TcpServer::Config& config = {});
  bool init() override;
  void start() override;
  void stop() override;
  void get(std::string_view path, Handler handler);
  void post(std::string_view path, Handler handler);
  void put(std::string_view path, Handler handler);
  void del(std::string_view path, Handler handler);
  uint16_t actual_port() const override;
};
```

## 文件清单

| 路径 | 说明 |
|------|------|
| `net/http/include/router.h` | 路由器声明 |
| `net/http/src/router.cpp` | 路由器实现（trie）|
| `net/http/include/http_server.h` | HTTP 服务器声明（已存在，本步完善）|
| `net/http/src/http_server.cpp` | HTTP 服务器实现 |
| `net/http/include/i_router.h` | 路由抽象接口 |
| `net/http/include/i_http_server.h` | HTTP 服务器抽象接口 |
| `net/http/include/http_request.h` | HTTP 请求结构体（已存在，本步扩展 path_params）|
| `net/http/include/http_response.h` | HTTP 响应结构体（已存在）|
| `net/http/include/case_insensitive.h` | 大小写不敏感 HeaderMap |

## 测试用例

| 测试文件 | 用例数 | 覆盖场景 |
|---------|--------|---------|
| `test_router.cpp` | 9 | 静态路由、参数路由、404、方法不匹配 |
| `test_http_server.cpp` | 7 | GET/POST/PUT/DELETE、404、keep-alive、畸形请求、handler 异常 |

## 质量门禁

| 检查项 | 结果 |
|--------|------|
| clang-tidy | ✅ 0 / 0 / 0 |
| cppcheck | ✅ 0 / 0 / 0 / 0 |
| 编译 | ✅ 0 error / 0 warning |
| 测试 | ✅ 16/16 通过 |
