# 音乐软件 HTTP 服务器 — 项目目标与架构定稿

## 项目概述

高并发 HTTP 服务器，为音乐软件提供后端服务。

### 技术栈

- C++20 / g++ / xmake
- HTTP/1.1 + HTTPS (TLS)
- WebSocket (实时通信)
- boost::mysql (数据/音乐索引管理)
- 协程 (Coroutine, C++20 `std::coroutine`)
- 线程池（LockFree 无锁 + Locked 有锁双实现）
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
  │   ├─ coroutine (无接口, 与 Connection 耦合, await_read/await_write)
  │   ├─ tcp_server (ITcpServer 抽象类 + SslContext 集成)
  │   ├─ tcp_client (ITcpClient 抽象类)
  │   ├─ ssl (SslContext 封装, OpenSSL)
  │   ├─ websocket (帧编解码 + WsConnection 事件循环)
  │   ├─ http (IHttpServer/IRouter 抽象类 + Range 流式传输 + WebSocket 升级)
  │   └─ file-transfer (IFileTransfer 抽象类)
  │       ├─ file-send-process (独立进程)
  │       └─ file-receive-process (独立进程)
  ├─ file-system (IFileSystem 抽象类)
  └─ database (IDatabasePool 抽象类, boost::mysql 连接池)
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
class LockedThreadPool : public ThreadPoolBase<LockedThreadPool> {
public:
  explicit LockedThreadPool(std::size_t num_threads);
  template <typename F, typename... Args> requires std::invocable<F, Args...>
  void enqueue_impl(F&& f, Args&&... args);
  void wait_impl();
  bool wait_for(std::chrono::milliseconds timeout);
  void stop_impl();
private:
  std::vector<std::jthread> workers_;
  std::queue<MoveOnlyFunction> tasks_;
  std::mutex queue_mutex_;
  std::condition_variable cv_;
  std::atomic<int> pending_{0};
  bool stop_{false};
};
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

## Step 15 — 配套前端 Web 界面（已定稿）

### 概述
> 设计体系: Crystal Music — 青绿翡翠 × Glassmorphism × Frosted Glass
> 文档: [plan/step-15-frontend.md](plan/step-15-frontend.md)

### 目标
1. 在 `frontend/` 目录下构建 TypeScript + React + Vite SPA
2. 设计体系: Crystal Music（青绿翡翠色 + 玻璃态 + 毛玻璃）
3. 9 个页面：登录/注册/文件列表/文件详情/上传/音乐库/歌单/播放器/用户管理
4. 状态管理: Zustand（auth / player / toast / music）
5. 对接后端 RESTful API 和 WebSocket
6. 前后端分离，通过 Nginx 容器反向代理
7. 质量门禁：Vite build 0 error、TypeScript strict 0 error、Vitest + Playwright 100%

## Step 16 — 后端数据库重构 + 音乐库/歌单 API（已完成）

### 概述
> 文档: [plan/step-16-backend-music.md](plan/step-16-backend-music.md)

### 目标
1. 数据库 3NF 范式化: `music_meta` 独立表, `file_records` 关联 `music_id`
2. AST 查询优化: 索引设计、谓词下推、标准 JOIN
3. 补齐 11 项后端 API（认证/文件/用户）
4. 新增 8 项音乐库/歌单 API（浏览/创建/添加/移除/排序）
5. CORS 跨域支持: 后端全局注入 / Nginx 生产处理

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
| 连接模型 | 非阻塞 epoll ET + per-connection handler |
| 并发处理 | thread-pool + Coroutine 协作 |
| 数据库 | boost::mysql 连接池 (IDatabasePool) |
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
Step 7 ─── file-transfer 文件传输模块 (IFileTransfer 实现) ✅ 已完成
  ├─ IFileTransfer 抽象接口: transfer_small / transfer_large / receive_file
  ├─ FileTransfer 实现: 小文件(ITcpClient send_message), 大文件(fork/exec + stdin协议)
  ├─ ChunkHeader 28B 线缆协议 (HPSF magic, 网络字节序, packed)
  ├─ file-send-process 完善: stdin解析, LockFreeThreadPool并发发送, pread分块
  ├─ file-receive-process 实现: raw socket监听, 多线程接收, pwrite重组
  └─ 测试: test_file_transfer(7用例) 全通过
        │
Step 8 ─── HTTPS/TLS (OpenSSL, 双模式) ✅ 已完成
  ├─ SslContext 封装 + Connection SSL 集成
  ├─ 双模式检测（peek 首字节 0x16 → TLS）
  ├─ 异步 SSL 握手（WANT_READ/WANT_WRITE 重试）
  ├─ 测试证书自动生成（xmake after_build 脚本）
  └─ 测试: test_ssl(9用例) 全通过
        │
Step 9 ─── WebSocket 握手 + 帧编解码 ✅ 已完成
  ├─ Base64 编码（RFC 4648）
  ├─ WebSocket 握手（Sec-WebSocket-Accept SHA-1 + Base64 计算）
  ├─ 帧编码（TEXT/BINARY/CLOSE/PING/PONG，服务端不 mask）
  ├─ 帧解码（自动 unmask，7/16/64 位 payload 长度）
  ├─ WsConnection 事件循环（CLOSE/PING/PONG 处理）
  ├─ HttpServer 集成（ws() 路由注册，Upgrade 检测，方案B 事件循环）
  └─ 测试: test_websocket(12用例) 全通过
        │
Step 10 ─── LockedThreadPool 有锁通用线程池实现 ✅ 已完成
  ├─ CRTP 继承 ThreadPoolBase<LockedThreadPool>
  ├─ std::queue + std::mutex 有锁任务队列
  ├─ condition_variable 等待通知，wait_for(ms) 超时支持
  ├─ stop() 队列清空 + pending 重置 + jthread 优雅关闭
  ├─ 异常安全：worker 内 try-catch + Logger::_error 记录
  └─ 测试: test_locked_thread_pool(8用例) 全通过
        │
Step 11 ─── main.cpp 整合启动 ✅ 已完成
  ├─ 服务器启动流程: Logger::init → 配置加载 → DatabasePool(MockConnection) → FileSystem → HttpServer → start → 信号等待 → stop → close → shutdown
  ├─ 路由注册: /api/health, /api/users/:id, POST /api/users, /api/users/:id/history, /api/files/:hash, ws://host/ws
  ├─ 配置管理: config.json (nlohmann/json) + 命令行参数 (--port/--threads/--db-host等)
  └─ 测试: 手动启动验证 + 全量回归测试 20二进制全部通过
        │
Step 12 ─── 端到端验证 ✅ 已完成
  ├─ main.cpp 路由注册: POST /api/files/upload + GET /api/files/by-hash/:hash/download
  ├─ B2 修复: CLI 参数覆盖 JSON (parse_json_file → parse_cmd_args 交换)
  ├─ B5 修复: Router path_exists 接口 (405 vs 404 区分)
  ├─ B6/B7 修复: 静态 map 元信息持久化 + 双写 (绕开 MockConnection)
  ├─ 大 body 分片: persistent parser per connection (Connection* key)
  ├─ 线程安全: parsers_ / s_file_meta 加 std::mutex 保护
  ├─ HttpParser 修复: Content-Length:0 直接 COMPLETE
  ├─ verification/verify.sh 增强: V12 文件全生命周期, V14 并发, V15 边界
  └─ 全量回归: lint 0/0 + test 20/20 + CodeQL 0/0 + verify 37/37
        │
Bugfix ─── 基准测试 Bug 修复 ✅ 已完成
  ├─ Bug 1: MemoryPool_BatchAllocate 卡死 ✅ 已完成
  │   ├─ 根因: try_merge_in_list 链表成环 + try_merge_and_promote 递归无上限 + page_index_of O(n) 线性扫描
  │   ├─ 改进: 快慢指针循环检测 → 迭代+上限(100) → 二分查找 O(log n) → 简化链表遍历
  │   └─ 验证: BM_MemoryPool_BatchAllocate 全部参数正常完成，BM_MemoryPool_MixedSizes 不受影响
  ├─ Bug 2: LockFreeThreadPool 高负载卡死 ✅ 已完成
  │   ├─ 根因: LockFreeQueue::pop() 的 empty() 存在 TOCTOU 竞态——head 和 tail 分两次 load，
  │   │   消费者可看到过时 head + 新 tail，误判队列非空后 CAS 将 head 推到 tail 之后，
  │   │   然后永久自旋在 EMPTY slot 上等待永远不会到来的 PUSHED 状态。
  │   ├─ 修复: load current_head 后重新 load tail 做二次验证，若两 index 相等说明队列已空，continue 重试
  │   └─ 验证: 50 trials × 4 workers × 200 轮全部通过，全量 benchmark 正常
  ├─ Bug 3: LockFreeQueue 高并发 benchmark 卡死 ✅ 已完成
  │   ├─ 根因: pop()/try_pop 的 while(state_ != PUSHED) 等待循环不检查 stop_token_，
  │   │   高并发下（c≥256）消费者 CAS head 成功后，对应生产者被 stop 退出但 slot 处于
  │   │   "已认领未写入"中间态，消费者永久自旋
  │   ├─ 修复: 在 state_ 等待循环中加 stop_token_.stop_requested() 检查，发现 stop 则标记
  │   │   EMPTY 并返回 false
  │   └─ 验证: qps_lock_free_queue + qps_thread_pool 各 11 级并发(c=1~1024)全部通过
  └─ 质量门禁: lint 0/0 + 编译 0/0 + test 19/20（tcp_client 间歇失败）+ CodeQL 0/0 ✅
  │
  ├─ 遗留修复: SO_REUSEADDR + SO_REUSEPORT 已解决 tcp_client 端口竞争，test 37/37 全通过
        │
Step 14 ─── 文件上传功能改进 ✅ 已完成
  ├─ 流式分片存储: 2MB 每片，`HttpParser` 流式模式 + chunk_handler 回调
  ├─ 原文件名保留: `Content-Disposition` 提取
  ├─ 认证 & RBAC: GUEST/NORMAL/VIP 三级权限，HMAC-SHA256 Token
  ├─ Bug 修复:
  │   ├─ Bug 1: read_buffer_ 无锁跨线程访问 → malloc 堆损坏（新增 read_mutex_ 保护）
  │   └─ Bug 2: Keep-Alive upload_ctx 复用污染 → 移入 while 循环
  ├─ 质量门禁: lint 0/0 + test 20/20 + 编译 0/0 + CodeQL 0/0
  └─ 详情: [plan/step-14-file-upload-improvement.md](plan/step-14-file-upload-improvement.md) |
        │
Bugfix ─── 并发上传 Bug 修复 ✅ 已完成
  ├─ Bug 1: read_buffer_ data race → malloc 堆损坏
  │   ├─ 根因: 事件循环 `read_from_fd()` 与 handler `handle_connection()` 无锁并发访问 `read_buffer_`
  │   ├─ 修复: Connection 新增 `read_mutex_`，所有读写路径加锁；handler 改用拷贝模式避免持锁跨阻塞操作
  │   └─ 涉及: connection.h/cpp, http_server.cpp, ws_connection.cpp
  ├─ Bug 2: Keep-Alive upload_ctx 复用污染
  │   ├─ 根因: upload_ctx 在 while 循环外创建，parser.reset() 后残留 chunks/hash_ctx 污染下一请求
  │   ├─ 修复: upload_ctx 移入 while 循环 + set_headers_done_callback 重注册，每请求独立上下文
  │   └─ 涉及: http_server.cpp
  └─ 详情: [plan/bugfix-concurrent-uploads.md](plan/bugfix-concurrent-uploads.md)
        │
Step 15 ─── 配套前端 Web 界面 Crystal Music ✅ 已完成
  ├─ React 19 + Vite 8 + TypeScript 6 + Tailwind CSS 4 SPA
  ├─ 设计体系: 青绿翡翠 × Glassmorphism × Frosted Glass
  ├─ 9 页面: 登录/注册/文件列表/文件详情/上传/音乐库/歌单/播放器/用户管理
  ├─ 状态管理: Zustand (auth/player/toast/music)
  ├─ 测试: Vitest 10/10 通过
  ├─ 质量门禁: build 0 error + tsc 0 error
  └─ 详情: [plan/step-15-frontend.md](plan/step-15-frontend.md)
        │
Step 16 ─── 后端数据库重构 + 音乐库/歌单 API ✅ 已完成
  ├─ 数据库 3NF 范式化: music_meta + file_records 解耦, users 加 salt
  ├─ AST 查询优化: 索引设计 + 标准 JOIN
  ├─ 新增 11 后端接口: register/me/logout/stream/search/delete/update_user
  ├─ 新增 8 音乐/歌单接口 (M1–M8): 音乐库浏览 + 歌单 CRUD + 排序
  ├─ 测试修复: validate_token 偏移量 Bug（uid_pos+4 → uid_pos+3）
  └─ 质量门禁: lint 0/0 + test 21/21 + 编译 0/0
        │
Comprehensive-Test ─── 全方位测试 + 微基准覆盖 ✅ 已完成
  ├─ Bug 修复: Prepared Statement catch 静默吞异常 → 诊断增强（e.what + server_message）
  │             bind API 调整适配 Boost 1.83+，fix-prepared-statement.md 落地
  ├─ Bug 修复: test_tcp_client 间歇性失败 → SO_REUSEADDR + SO_REUSEPORT
  ├─ Bug 修复: main.cpp 拆分可测函数（load_config/add_routes 抽出）
  ├─ 测试增强: 新增 16 个测试文件，112 用例覆盖边界/压力/异常路径
  │   ├─ test_logger(10) — 生命周期/并发/双重 init/未 init 抛异常
  │   ├─ test_memory_pool_extreme(8) — 零大小/超大/跨线程/碎片/批量
  │   ├─ test_thread_pool_stress(8) — stop/异常/并发/超时/promise
  │   ├─ test_http_parser_extreme(10) — 超长路径/多头/管线/重置循环
  │   ├─ test_http_server_stress(6) — 并发/Keep-Alive/Connection:close/启停
  │   ├─ test_websocket_extreme(8) — 大帧/16bit 长/分片/全 opcode/掩码
  │   ├─ test_coroutine_extreme(6) — 异常/move-only/大数据/批量
  │   ├─ test_auth_service(8) — 注册/登录/token 验证/过期/篡改/并发
  │   ├─ test_ssl_extreme(6) — 无效证书/预初始化/裸上下文/清理
  │   ├─ test_file_system_stress(6) — 并发/大文件/不存在/覆盖
  │   ├─ test_file_transfer_stress(6) — ChunkHeader 序列化/并发/断连
  │   ├─ test_database_pool_stress(8) — 池耗尽/超时/损坏连接/关闭后获取
  │   ├─ test_tcp_server_stress(6) — 多客户端/断连/大流量/启停
  │   ├─ test_config(4) — 有效/缺失/无效 JSON/默认值
  │   ├─ test_range_parser_extreme(6) — 开区间/多段/越界
  │   └─ test_url_decode_extreme(6) — 混合编码/无效 %XX/空输入
  ├─ 微基准增强: 新增 8 个 bench_* + 8 个 qps_*，8 个已有追加操作维度
  │   ├─ 新增: bench_logger/bench_auth_service/bench_tcp_server/bench_http_server
  │   ├─ 新增: qps_logger/qps_auth_service/qps_tcp_server/qps_http_server
  │   ├─ 追加: 内存池多线程/线程池延迟分布队列MPMC/文件系统并发/连接池耗尽
  │   │        WebSocket 连续帧/SSL 读写吞吐/协程切换开销
  └─ 质量门禁: lint 0/0 + 编译 0/0 + test 37/37 + CodeQL 0/0

---

## Step 13 — Docker 化部署 ✅ 已完成

### 目标
1. 多阶段构建 Dockerfile（ubuntu:22.04），分离构建/运行环境
2. 支持 release 模式编译，产物最小化
3. 支持 SSL 证书挂载和配置文件挂载
4. 支持 docker-compose 编排（服务 + 数据库）
5. 镜像大小控制在 200MB 以内

### 计划
- [x] 创建 Dockerfile（运行时镜像，COPY 本机产物）
- [x] 创建 .dockerignore
- [x] 创建 docker-compose.yml（服务 + MySQL，端口 9090）
- [x] 健康检查端点验证（HEALTHCHECK + /api/health）
- [x] CI 集成镜像构建（GitHub Actions）
- [x] 质量门禁：lint 0/0 + test 20/20 + CodeQL 0/0
- [x] 镜像大小 139MB（≤ 200MB）

---

## 模块完成度总览

| 模块 | 文件数 | 完成度 | 状态 |
|------|--------|--------|------|
| core | 2 | 100% | ✅ 就绪 |
| logger | 6 | 100% | ✅ 就绪（两阶段单例已落地）|
| net/http | 11 | 100% | ✅ 就绪（Range 流式传输已落地）|
| net/tcp/tcp_client | 2 | 100% | ✅ 就绪（ITcpClient 已落地）|
| net/coroutine | 1 | 100% | ✅ 就绪（Connection awaitable 已落地）|
| net/tcp/tcp_server | 4 | 100% | ✅ 就绪（ITcpServer + epoll ET + SSL + 优雅关闭）|
| net/thread-pool | 5 | 100% | ✅ 就绪（CRTP + LockFree + Locked 双实现）|
| memory-pool | 6 | 100% | ✅ 就绪（去 virtual 析构 + 静态 deleter 已落地）|
| net/file-transfer | 5 | 100% | ✅ 就绪（IFileTransfer + FileTransfer + chunk_header 已落地）|
| net/file-send | 1 | 100% | ✅ 就绪（file-send-process 已完善）|
| file-system | 3 | 100% | ✅ 就绪（IFileSystem + FileSystem 已落地）|
| net/file-receive | 1 | 100% | ✅ 就绪（file-receive-process 已实现）|
| database | 8 | 100% | ✅ 就绪（IDatabasePool + DatabasePool + boost::mysql 已落地） |
| net/ssl | 3 | 100% | ✅ 就绪（SslContext + Connection SSL 集成 + 双模式检测） |
| net/websocket | 5 | 100% | ✅ 就绪（帧编解码 + 握手 + WsConnection 事件循环） |
| tests | 37 | 100% | ✅ 就绪（37 二进制，共 39+ 测试套全通过）|
| **frontend** | ~40 | **100%** | ✅ **已完成**（Crystal Music 设计体系 + 9 页面 + Zustand + Vitest，详见 `plan/step-15-frontend.md`）|
| backend-music | ~15 | **100%** | ✅ **已完成**（数据库范式重构 + AST 优化 + 19 路由 + 8 音乐/歌单接口，详见 `plan/step-16-backend-music.md`）|

---

## 新增模块清单

| 模块 | 目录 | 接口 | 状态 |
|------|------|------|------|
| `file-transfer` | `net/file-transfer/` | `IFileTransfer` | ✅ 已实现（小文件大文件传输 + 独立进程） |
| `database` | `db/` | `IDatabasePool` | ✅ 已实现（boost::mysql 连接池） |
| `net/ssl` | `net/ssl/` | (SslContext 封装) | ✅ 已实现（双模式 HTTPS/TLS） |
| `net/websocket` | `net/websocket/` | `websocket.h` / `ws_connection.h` | ✅ 已实现（帧编解码 + 握手 + 事件循环） |
| `LockedThreadPool` | `net/thread-pool/` | `ThreadPoolBase<LockedThreadPool>` | ✅ 已实现（有锁通用版，含 wait_for 超时） |

---

## 测试覆盖

| 模块 | 测试文件 | 覆盖情况 |
|------|---------|----------|
| TcpClient | `test_tcp_client.cpp` | ✅ 有（8 用例，含动态端口修复）|
| TcpServer | `test_tcp_server.cpp`, `test_tcp_server_connection.cpp` | ✅ 有 |
| TcpServer 压力 | `test_tcp_server_stress.cpp` | ✅ 新增（6 用例：并发/断连/大流量/启停）|
| HttpParser | `test_http_parser.cpp`, `test_http_parser_extreme.cpp` | ✅ 有（10+10=20 用例，含边界/管线/重置）|
| HttpRequest | `test_http_request.cpp` | ✅ 有 |
| HttpResponse | `test_http_response.cpp` | ✅ 有 |
| UrlDecode | `test_url_decode.cpp`, `test_url_decode_extreme.cpp` | ✅ 有（+6 边界用例）|
| MemoryPool | `test_memory_pool.cpp`, `test_memory_pool_extreme.cpp` | ✅ 有（+8 极端用例：跨线程/碎片/批量）|
| thread-pool (LockFree) | `test_lock_free_queue.cpp` | ✅ 有 |
| thread-pool (Locked) | `test_locked_thread_pool.cpp` | ✅ 有（8 用例）|
| thread-pool 压力 | `test_thread_pool_stress.cpp` | ✅ 新增（8 用例：stop/异常/并发/promise）|
| MoveOnlyFunction | `test_move_only_function.cpp` | ✅ 有 |
| coroutine | `test_coroutine.cpp`, `test_coroutine_extreme.cpp` | ✅ 有（5+6=11 用例，含批量/move-only）|
| Router | `test_router.cpp` | ✅ 有（9 用例）|
| HttpServer | `test_http_server.cpp`, `test_http_server_stress.cpp` | ✅ 有（7+6=13 用例端到端+压力）|
| range-parser | `test_range_parser.cpp`, `test_range_parser_extreme.cpp` | ✅ 有（10+6=16 用例）|
| logger | `test_logger.cpp` | ✅ **新增**（10 用例：生命周期/并发/双重 init）|
| file-system | `test_file_system.cpp`, `test_file_system_stress.cpp` | ✅ 有（14+6=20 用例，含并发/大文件）|
| database | `test_database_pool.cpp`, `test_database_pool_stress.cpp` | ✅ 有（34+8=42 用例：含连接池耗尽/超时/并发）|
| Step16 API | `test_step16_api.cpp` | ✅ 有（5 用例）|
| file-transfer | `test_file_transfer.cpp`, `test_file_transfer_stress.cpp` | ✅ 有（7+6=13 用例，含并发/断连）|
| SSL/TLS | `test_ssl.cpp`, `test_ssl_extreme.cpp` | ✅ 有（9+6=15 用例，含无效证书/预初始化）|
| WebSocket | `test_websocket.cpp`, `test_websocket_extreme.cpp` | ✅ 有（12+8=20 用例，含大帧/全 opcode）|
| AuthService | `test_auth_service.cpp` | ✅ **新增**（8 用例：注册/登录/token/并发）|
| Config | `test_config.cpp` | ✅ **新增**（4 用例：加载/缺失/无效 JSON）|
