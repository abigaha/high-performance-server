# 音乐软件 HTTP 服务器 — 项目目标与架构定稿

## 项目概述

高并发 HTTP 服务器，为音乐软件提供后端服务。

### 技术栈

- C++20 / g++ / xmake
- HTTP/1.1 + HTTPS (TLS)
- WebSocket (实时通信)
- MySQL (数据/音乐索引管理)
- 协程 (Coroutine, C++20 `std::coroutine`)
- 线程池 + 无锁队列 (LockFreeQueue)
- Range 流式传输 (音乐播放进度控制)

---

## 架构设计原则

1. **所有模块通过接口解耦**，消费者依赖接口而非具体实现
2. **高频调用路径用 CRTP 静态多态**（编译期内联，零虚函数开销）
3. **非热点路径用抽象类动态多态**（virtual，灵活性优先）
4. **依赖单向**，上层依赖下层接口，禁止循环依赖

---

## 模块总览与接口决策

| 模块 | 接口 | 多态方式 | 理由 |
|------|------|---------|------|
| logger | `Logger`（两阶段强限制单例） | 无接口类 | 完备工具模块；init/shutdown 控制生命周期覆盖所有模块 |
| memory-pool | `MemoryPoolBase<Derived>` | CRTP + 静态析构 deleter | 协程 new/delete 高频；析构通过类型擦除 deleter 静态分发 |
| thread-pool | `ThreadPoolBase<Derived>` | CRTP | enqueue 高频；预留两实现切换 |
| coroutine | 无接口 | 模板类 | 单文件，与 Connection 耦合提供协程化 IO |
| tcp_server | `ITcpServer` | 抽象类 | 连接创建/释放非热点 |
| tcp_client | `ITcpClient` | 抽象类 | 同上 |
| http | `IHttpServer` / `IRouter` | 抽象类 | 请求处理非热点 |
| file-system | `IFileSystem` | 抽象类 | 文件切割/哈希/存储管理 |
| file-transfer | `IFileTransfer` | 抽象类 | 小文件单连接，大文件唤起进程并行 |
| database | `IDatabasePool` | 抽象类 | 用户/下载日志/文件哈希/存储地址管理 |

---

## 依赖拓扑

```
core (main)
  ├─ logger (两阶段: init/shutdown 强限制单例)
  ├─ memory-pool (CRTP: MemoryPoolBase<T> + 静态析构 deleter)
  ├─ thread-pool (CRTP: ThreadPoolBase<Derived>)
  │   ├─ LockFreeThreadPool (现有无锁版)
  │   └─ LockedThreadPool (新增有锁通用版)
  ├─ net
  │   ├─ coroutine (无接口, 与 Connection 耦合, await_transform)
  │   ├─ tcp_server (ITcpServer 抽象类)
  │   ├─ tcp_client (ITcpClient 抽象类)
  │   ├─ http (IHttpServer/IRouter 抽象类)
  │   └─ file-transfer (IFileTransfer 抽象类)
  │       ├─ file-send-process (独立进程)
  │       └─ file-receive-process (独立进程)
  ├─ file-system (IFileSystem 抽象类)
  └─ database (IDatabasePool 抽象类, 新增)
```

---

## 各模块详细设计

### 1. logger — 两阶段强限制单例

```cpp
class Logger {
public:
  // 两阶段：main 入口 init，返回前 shutdown
  static void init(const std::string& name = "server");
  static void shutdown();
  // getInstance 强限制：未 init 抛异常
  static Logger& getInstance();
  // 静态便捷 API 保留 _ 前缀
  static void _info(const std::string& msg);
  // ...
private:
  Logger() = default;  // 私有，禁止外部构造
  static std::unique_ptr<Logger> s_instance_;
  static std::once_flag s_init_flag_;
  bool initialized_{false};
};
```

- `init()` 创建实例并配置默认 appender
- `shutdown()` 显式销毁，保证在所有模块析构后执行
- `getInstance()` 未 init 时抛 `std::runtime_error`（强限制，不静默 fallback）
- 现有 `Logger::_info()` 调用点无需改动

### 2. memory-pool — CRTP + 静态析构 deleter

```cpp
template <typename Derived>
class MemoryPoolBase {
public:
  void* allocate(std::size_t size) noexcept {
    return static_cast<Derived*>(this)->allocate_impl(size);
  }
  void deallocate(void* ptr, std::size_t size) noexcept {
    static_cast<Derived*>(this)->deallocate_impl(ptr, size);
  }
  // 无 virtual 析构！静态分发通过 deleter 实现
protected:
  MemoryPoolBase() = default;
  ~MemoryPoolBase() = default;  // protected，禁止直接 delete 基类指针
};

// 工厂返回 unique_ptr<基类> + 自定义 deleter（编译期已知具体类型）
auto CreateMemoryPool()
  -> std::unique_ptr<MemoryPoolBase<TieredMemoryPool>,
                     void(*)(MemoryPoolBase<TieredMemoryPool>*)>;

// deletor 实现：static_cast 到具体类型后 delete（静态分发，无 virtual）
inline void TieredMemoryPoolDeleter(MemoryPoolBase<TieredMemoryPool>* p) {
  delete static_cast<TieredMemoryPool*>(p);
}
```

- `allocate/deallocate`：CRTP 内联（高频路径零开销）
- 析构：通过 deleter `static_cast` 后 delete，调 `TieredMemoryPool` 析构链（静态分发，无 virtual）
- 消费者依赖 `MemoryPoolBase<TieredMemoryPool>&` 接口

### 3. thread-pool — CRTP 接口 + 两实现

```cpp
template <typename Derived>
class ThreadPoolBase {
public:
  template <typename F, typename... Args>
    requires std::invocable<F, Args...>
  void enqueue(F&& f, Args&&... args) {
    static_cast<Derived*>(this)->enqueue_impl(std::forward<F>(f), std::forward<Args>(args)...);
  }
  void wait_for_all_tasks() { static_cast<Derived*>(this)->wait_impl(); }
  void stop() { static_cast<Derived*>(this)->stop_impl(); }
protected:
  ThreadPoolBase() = default;
  ~ThreadPoolBase() = default;
};

// 实现 1：现有无锁版
class LockFreeThreadPool : public ThreadPoolBase<LockFreeThreadPool> {
public:
  template <typename F, typename... Args> requires std::invocable<F, Args...>
  void enqueue_impl(F&& f, Args&&... args);
  void wait_impl();
  void stop_impl();
private:
  LockFreeQueue<MoveOnlyFunction> tasks_;
  // ...
};

// 实现 2：新增有锁通用版
class LockedThreadPool : public ThreadPoolBase<LockedThreadPool> { ... };
```

### 4. coroutine — 无接口，与 Connection 耦合

- `CoroItem<T>` 保持模板类
- `Connection` 实现 awaitable 接口：
  ```cpp
  struct ReadAwaiter { bool await_ready(); void await_suspend(std::coroutine_handle<>); ssize_t await_resume(); };
  ReadAwaiter await_read(Connection& conn);
  ```
- 协程内 `co_await conn.read()` 实现非阻塞 IO

### 5. tcp_server / tcp_client — 抽象类接口

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
  virtual bool is_connected() const = 0;
};
```

### 6. http — 抽象类接口

```cpp
class IHttpServer {
public:
  virtual ~IHttpServer() = default;
  virtual bool init() = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void get(std::string_view path, Router::Handler h) = 0;
  virtual void post(std::string_view path, Router::Handler h) = 0;
  virtual void put(std::string_view path, Router::Handler h) = 0;
  virtual void del(std::string_view path, Router::Handler h) = 0;
  virtual uint16_t actual_port() const = 0;
};

class IRouter {
public:
  virtual ~IRouter() = default;
  virtual void add(HttpMethod m, std::string_view path, Router::Handler h) = 0;
  virtual bool match(HttpMethod m, std::string_view path, Router::Handler& out, Params& outParams) const = 0;
};
```

### 7. file-system — 抽象类接口

```cpp
class IFileSystem {
public:
  virtual ~IFileSystem() = default;
  // 文件切割
  virtual std::vector<FileChunk> split_file(const std::string& path, std::size_t chunk_size) = 0;
  // 完整哈希
  virtual std::string compute_file_hash(const std::string& path) = 0;
  // 分块哈希
  virtual std::string compute_chunk_hash(const FileChunk& chunk) = 0;
  // 存储管理
  virtual bool store_file(const std::string& path, const std::vector<char>& data) = 0;
  virtual bool delete_file(const std::string& path) = 0;
  virtual std::optional<std::vector<char>> read_file(const std::string& path) = 0;
};
```

### 8. file-transfer — 新增模块 + 抽象类接口

```cpp
class IFileTransfer {
public:
  virtual ~IFileTransfer() = default;
  // 小文件：单连接传输
  virtual bool transfer_small(const std::string& path, ITcpClient& client) = 0;
  // 大文件：唤起 file-send/receive 进程，切割并行传输
  virtual bool transfer_large(const std::string& path, const std::string& peer_ip, uint16_t peer_port) = 0;
  // 接收端
  virtual bool receive_file(const std::string& save_path, ITcpServer& server) = 0;
};
```

- `file-send-process` / `file-receive-process` 作为独立进程，由 `IFileTransfer` 实现通过 fork/exec 唤起

### 9. database — 新增模块 + 抽象类接口

```cpp
class IDatabasePool {
public:
  virtual ~IDatabasePool() = default;
  virtual bool init(const DbConfig& config) = 0;
  // 用户信息
  virtual std::optional<User> get_user(int64_t user_id) = 0;
  virtual bool create_user(const User& user) = 0;
  // 下载日志
  virtual bool log_download(const DownloadLog& log) = 0;
  virtual std::vector<DownloadLog> get_download_history(int64_t user_id) = 0;
  // 文件哈希与存储地址
  virtual bool store_file_meta(const FileMeta& meta) = 0;
  virtual std::optional<FileMeta> get_file_meta(const std::string& hash) = 0;
};
```

---

## 单例与生命周期

```
main() {
  Logger::init("server");        // 1. 最先初始化
  // ... 创建各模块（依赖注入或工厂）...
  // ... 运行服务器 ...
  // ... 各模块析构 ...
  Logger::shutdown();            // N. 最后销毁，覆盖所有模块资源释放日志
}
```

- Logger 由 `init/shutdown` 显式控制，保证生命周期覆盖所有模块
- 其他模块通过构造函数注入接口引用（DI），或通过工厂获取

---

## 关键设计决策

| 领域 | 决策 |
|------|------|
| HTTP 解析 | 手写轻量解析器 |
| 路由匹配 | 前缀树 (trie) |
| 连接模型 | 每个连接一个 Coroutine |
| 并发处理 | thread-pool + Coroutine 协作 |
| 数据库 | MySQL Connector/C++ 或 libmysqlclient |
| TLS | OpenSSL |
| 命名空间 | 全部统一到 `hps` |
| 命名规则 | 成员变量 `_` 后缀，全局变量 `g_` 前缀，常量 `k`+PascalCase，枚举 UPPER_SNAKE_CASE |

---

## 质量门禁（严格模式）

- clang-tidy：0 error + 0 warning + 0 style
- cppcheck --enable=all：0 error + 0 warning + 0 style + 0 performance
- CodeQL：0 critical + 0 high severity
- 编译：0 error + 0 warning
- 测试：100% 通过

---

## 续写进度

```
Step 0–2 ─── 基础设施质量优化 (已完成)
  ├─ clang-tidy 0/0/0 + cppcheck 0 + CodeQL 0/0
  ├─ thread-pool: bind → fold lambda, LockFreeQueue 指针安全
  ├─ HTTP Parser: 设计初始化器、认知复杂度拆分、C++20 兼容
  ├─ TcpClient: 非阻塞 + poll 超时 + 粘性缓冲区 + 移动语义 + 测试
  └─ TcpServer: 数组安全、认知复杂度拆分、epoll 封装、[[maybe_unused]]
        │
Step 2 ─── 路由注册 + 请求分发 (已完成)
  ├─ Router: 前缀树(trie)路由，静态段+参数段(:name)，静态优先+回溯
  ├─ HttpServer: 封装 TcpServer+Router，per-conn 局部 parser，keep-alive
  ├─ HttpRequest 扩展 path_params，错误响应(400/404/413/500)，handler 异常捕获
  └─ 测试: test_router(9用例) + test_http_server(7用例端到端) 全通过
        │
Step 2.5 ─── 架构重构 (已完成)
  ├─ 命名规范统一（类名去C前缀、snake_case方法、k+PascalCase常量等）
  └─ 内存池 CRTP 静态多态 + MoveOnlyFunction SBO 优化
        │
Step 3 ─── 架构改造：接口层落地 (已完成)
  ├─ logger: 两阶段 init/shutdown 强限制单例
  ├─ memory-pool: 去 virtual 析构，改 deleter 静态分发
  ├─ thread-pool: 提取 CRTP 基类，现有实现改名 LockFreeThreadPool
  ├─ tcp_server/tcp_client: 提取 ITcpServer/ITcpClient 抽象类
  ├─ http: 提取 IHttpServer/IRouter 抽象类
  └─ coroutine: Connection 加 awaitable 支持（await_read/await_write + 3 测试）
        │
Step 4 ─── file-system 文件读取 + 虚拟路径解析 (IFileSystem 实现) (已完成)
  ├─ IFileSystem 抽象接口 + FileChunk 结构体
  ├─ FileSystem 实现: split_file/compute_file_hash/compute_chunk_hash
  ├─ 存储管理: store_file/read_file/delete_file + 自动建目录
  ├─ 虚拟路径解析: resolve_path + 路径穿越防护
  └─ 测试: test_file_system(14用例) 全通过
        │
Step 5 ─── Range 流式传输 (解析、206 Partial Content) ✅ 已完成
        │
Step 6 ─── database 数据库连接池 (IDatabasePool 实现) ✅ 已完成
  ├─ 数据模型: DbConfig/User/DownloadLog/FileMeta
  ├─ IConnection 抽象接口 + BoostMySqlConnection (boost::mysql)
  ├─ DatabasePool: mutex+condition_variable 连接池，ping 健康检查，超时等待，close 等待活跃连接
  ├─ MockConnection 注入，无真实数据库可测
  ├─ Prepared statement 参数化查询防 SQL 注入
  ├─ SQL schema DDL
  └─ 测试: test_database_pool(14用例) 全通过
        │
Step 7 ─── file-transfer 文件传输模块 (IFileTransfer 实现)
  ├─ 小文件单连接传输
  ├─ 大文件唤起 file-send/receive 进程切割并行传输
  └─ file-send-process / file-receive-process 进程实现
        │
Step 8 ─── HTTPS/TLS (OpenSSL, 双模式)
        │
Step 9 ─── WebSocket 握手 + 帧编解码
        │
Step 10 ─── LockedThreadPool 有锁通用线程池实现
        │
Step 11 ─── main.cpp 整合启动
  ├─ 服务器启动流程
  ├─ 路由注册
  └─ 配置管理
```

---

## 模块完成度总览

| 模块 | 文件数 | 完成度 | 状态 |
|------|--------|--------|------|
| core | 2 | 100% | ✅ 就绪 |
| logger | 6 | 100% | ✅ 就绪（两阶段单例已落地）|
| net/http | 11 | 100% | ✅ 就绪（Range 流式传输已落地）|
| net/tcp/tcp_client | 2 | 100% | ✅ 就绪（ITcpClient 已落地）|
| net/coroutine | 1 | 100% | ✅ 就绪（Connection awaitable 已落地）|
| net/tcp/tcp_server | 4 | 90% | ✅ 就绪（ITcpServer 已落地）|
| net/thread-pool | 3 | 90% | ✅ 就绪（CRTP 基类 + LockFreeThreadPool 改名已落地）|
| memory-pool | 6 | 100% | ✅ 就绪（去 virtual 析构 + 静态 deleter 已落地）|
| net/file-send | 1 | 40% | ❌ 未完成 |
| file-system | 3 | 100% | ✅ 就绪（IFileSystem + FileSystem 已落地）|
| net/file-receive | 1 | 0% | 🚫 空桩 |
| **database** | **8** | **100%** | **✅ 就绪（IDatabasePool + DatabasePool + boost::mysql 已落地）** |
| tests | 16 | 100% | ✅ 就绪（53 用例全通过）|

---

## 新增模块清单

| 模块 | 目录 | 接口 | 状态 |
|------|------|------|------|
| `file-transfer` | `net/file-transfer/` | `IFileTransfer` | 待新增 |
| `database` | `db/` | `IDatabasePool` | ✅ 已实现（boost::mysql 连接池） |
| `LockedThreadPool` | `net/thread-pool/` | `ThreadPoolBase<LockedThreadPool>` | 待新增 |

---

## 测试覆盖

| 模块 | 测试文件 | 覆盖情况 |
|------|---------|----------|
| TcpClient | `test_tcp_client.cpp` | ✅ 有 |
| TcpServer | `test_tcp_server.cpp`, `test_tcp_server_connection.cpp` | ✅ 有 |
| HttpParser | `test_http_parser.cpp` | ✅ 有 |
| HttpRequest | `test_http_request.cpp` | ✅ 有 |
| HttpResponse | `test_http_response.cpp` | ✅ 有 |
| UrlDecode | `test_url_decode.cpp` | ✅ 有 |
| MemoryPool | `test_memory_pool.cpp` | ✅ 有 |
| thread-pool | `test_lock_free_queue.cpp` | ✅ 有 |
| MoveOnlyFunction | `test_move_only_function.cpp` | ✅ 有 |
| coroutine | `test_coroutine.cpp` | ✅ 有（5 用例：异常传播/正常完成/await_read 读/await_read EAGAIN/await_write）|
| Router | `test_router.cpp` | ✅ 有（9 用例）|
| HttpServer | `test_http_server.cpp` | ✅ 有（7 用例端到端）|
| range-parser | `test_range_parser.cpp` | ✅ 有（10 用例）|
| logger | — | ❌ 无 |
| file-system | `test_file_system.cpp` | ✅ 有（14 用例：split/hash/store/delete/路径穿越）|
| database | `test_database_pool.cpp` | ✅ 有（14 用例：模型/连接池/超时/CRUD）|
| file-send/receive | — | ❌ 无（模块未完成）|
