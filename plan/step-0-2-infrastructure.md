# Step 0–2：基础设施质量优化

> **状态**：✅ 已完成（commit `1102145` 等）
> **起止**：项目初始化阶段

## 背景

项目初建，完成基础模块开发与质量修复，确保 clang-tidy / cppcheck / CodeQL 全部达标。

## 功能点

| # | 功能点 | 说明 |
|---|--------|------|
| F1 | **clang-tidy 0/0/0** | 消除所有 error / warning / style 告警 |
| F2 | **cppcheck 0/0/0/0** | 消除所有 error / warning / style / performance 告警 |
| F3 | **CodeQL 0/0** | 消除所有 critical / high 问题 |
| F4 | **thread-pool 优化** | bind → fold lambda，LockFreeQueue 指针安全 |
| F5 | **HTTP Parser** | 设计初始化器、认知复杂度拆分、C++20 兼容 |
| F6 | **TcpClient** | 非阻塞 + poll 超时 + 粘性缓冲区 + 移动语义 + 测试 |
| F7 | **TcpServer** | 数组安全、认知复杂度拆分、epoll 封装、`[[maybe_unused]]` |

## 接口设计

本步骤完成后各模块接口已成型，后续步骤中逐步接口化：

### thread-pool

```cpp
class ThreadPool {
public:
  explicit ThreadPool(std::size_t num_threads);
  ~ThreadPool();

  template <typename F, typename... Args>
  void enqueue(F&& f, Args&&... args);

  void wait_for_all_tasks();
  void stop();
};
```

### HttpParser

```cpp
class HttpParser {
public:
  HttpParser();
  ParserResult feed(std::string_view data);
  ParserState state() const noexcept;
  ParserError error() const noexcept;
  const HttpRequest& request() const noexcept;
  void reset();
};
```

### TcpClient

```cpp
class TcpClient {
public:
  TcpClient(const std::string& host, uint16_t port);
  bool connect_to_server();
  void disconnect();
  bool send_message(const std::string& msg) const;
  bool receive_message(std::string& msg, ReadMode mode, uint32_t timeout_ms);
  bool is_connected() const;
};
```

### TcpServer

```cpp
class TcpServer {
public:
  struct Config { uint16_t port; int backlog; int worker_threads; int timeout_s; };
  explicit TcpServer(const Config& config);
  bool init();
  void start();
  void stop();
  uint16_t actual_port() const;
  void set_handler(Handler handler);
};
```

## 文件清单

| 路径 | 说明 |
|------|------|
| `net/thread-pool/include/thread_pool.h` | 线程池 |
| `net/thread-pool/include/lock_free_queue.hpp` | 无锁队列 |
| `net/thread-pool/src/thread_pool.cpp` | 线程池实现 |
| `net/http/include/http_parser.h` | HTTP 解析器 |
| `net/http/src/http_parser.cpp` | 解析器实现 |
| `net/tcp/tcp_client/include/tcp_client.h` | TCP 客户端 |
| `net/tcp/tcp_client/src/tcp_client.cpp` | 客户端实现 |
| `net/tcp/tcp_server/include/tcp_server.h` | TCP 服务器 |
| `net/tcp/tcp_server/src/tcp_server.cpp` | 服务器实现 |
| `net/tcp/tcp_server/include/connection.h` | 连接对象 |
| `net/tcp/tcp_server/src/connection.cpp` | 连接实现 |

## 测试用例

| 模块 | 测试文件 | 用例数 |
|------|---------|--------|
| TcpClient | `test_tcp_client.cpp` | 多用例 |
| TcpServer | `test_tcp_server.cpp` | 多用例 |
| HttpParser | `test_http_parser.cpp` | 多用例 |

## 质量门禁

| 检查项 | 结果 |
|--------|------|
| clang-tidy | ✅ 0 error / 0 warning / 0 style |
| cppcheck | ✅ 0 / 0 / 0 / 0 |
| CodeQL | ✅ 0 critical / 0 high |
| 编译 | ✅ 0 error / 0 warning |
| 测试 | ✅ 100% passing |
