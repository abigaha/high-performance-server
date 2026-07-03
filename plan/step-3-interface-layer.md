# Step 3：架构改造 — 接口层落地

> **状态**：✅ 已完成（commit `8d88538` + `911f648`）
> **起止**：架构重构后

## 背景

将所有模块通过接口解耦，消费者依赖接口而非具体实现。遵循 goal.md 中「高频调用路径用 CRTP 静态多态，非热点路径用抽象类动态多态」的原则。

## 功能点

| # | 功能点 | 说明 |
|---|--------|------|
| F1 | **logger 两阶段单例** | init/shutdown 强限制，std::call_once 线程安全，未 init 调用抛异常 |
| F2 | **memory-pool 去 virtual 析构** | CRTP 基类 + deleter 静态分发，零虚函数开销 |
| F3 | **thread-pool CRTP 基类** | 提取 ThreadPoolBase<Derived>，现有实现改名 LockFreeThreadPool |
| F4 | **tcp_server/tcp_client 抽象类** | ITcpServer / ITcpClient 抽象接口 |
| F5 | **http 抽象类** | IHttpServer / IRouter 抽象接口 |
| F6 | **coroutine awaitable** | Connection 加 await_read/await_write 支持 |

## 接口设计

### Logger

```cpp
class Logger {
public:
  static void init(const std::string& name = "server");
  static void shutdown();
  static Logger& getInstance();
  static void _info(const std::string& msg);
private:
  static std::unique_ptr<Logger> s_instance_;
  static std::once_flag s_init_flag_;
};
```

### ThreadPoolBase (CRTP)

```cpp
template <typename Derived>
class ThreadPoolBase {
public:
  template <typename F, typename... Args>
    requires std::invocable<F, Args...>
  void enqueue(F&& f, Args&&... args);
  void wait_for_all_tasks();
  void stop();
};
```

### ITcpServer / ITcpClient

```cpp
class ITcpServer {
public:
  virtual ~ITcpServer() = default;
  virtual bool init() = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void set_handler(Handler handler) = 0;
  virtual uint16_t actual_port() const = 0;
};

class ITcpClient {
public:
  virtual ~ITcpClient() = default;
  virtual bool connect_to_server() = 0;
  virtual void disconnect() = 0;
  virtual bool send_message(const std::string& msg) const = 0;
  virtual bool receive_message(std::string& msg, ReadMode mode, uint32_t timeout_ms) = 0;
};
```

## 文件清单

| 路径 | 操作 |
|------|------|
| `logger/include/logger.h` | 重构为两阶段单例 |
| `logger/src/logger.cpp` | 实现重构 |
| `memory-pool/include/memory_pool_base.h` | CRTP 去 virtual |
| `memory-pool/src/tiered_memory_pool.cpp` | 静态 deleter |
| `net/thread-pool/include/thread_pool_base.h` | CRTP 基类（新增）|
| `net/thread-pool/include/thread_pool.h` | 改名 LockFreeThreadPool |
| `net/thread-pool/src/thread_pool.cpp` | 适配新基类 |
| `net/tcp/tcp_server/include/i_tcp_server.h` | 抽象接口（新增）|
| `net/tcp/tcp_client/include/i_tcp_client.h` | 抽象接口（新增）|
| `net/http/include/i_http_server.h` | 抽象接口 |
| `net/http/include/i_router.h` | 抽象接口 |
| `net/coroutine/coroitem.hpp` | awaitable 支持 |

## 测试用例

| 测试文件 | 用例数 | 新增内容 |
|---------|--------|---------|
| `test_coroutine.cpp` | +3 | await_read 读 / await_read EAGAIN / await_write |

## 质量门禁

| 检查项 | 结果 |
|--------|------|
| clang-tidy | ✅ 0 / 0 / 0 |
| cppcheck | ✅ 0 / 0 / 0 / 0 |
| 编译 | ✅ 0 error / 0 warning |
| 测试 | ✅ 100% passing |
