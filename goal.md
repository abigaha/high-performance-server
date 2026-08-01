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

## Step 15 — 配套前端 Web 界面（历史第一版）

### 概述

> [plan/step-15-frontend.md](plan/step-15-frontend.md) 记录第一版前端的设计与实施基线。其依赖版本、测试数量、Playwright 完成状态和部分交互结论均属于历史快照；当前实现与验收要求以 Step 17 为准。

### 历史目标

1. 在 `frontend/` 目录下构建 TypeScript、React 与 Vite SPA。
2. 提供认证、文件、上传、音乐库、歌单、播放器和用户相关页面。
3. 使用 Zustand 管理认证、播放器、通知和音乐数据状态。
4. 对接后端 REST API 和 WebSocket，通过 nginx 提供统一公共入口。

第一版没有形成可复现的真实浏览器验收闭环，因此不得继续把其 Playwright 项目标记为已通过。

## Step 16 — 后端数据库重构 + 音乐库/歌单 API（已完成）

### 概述
> 文档: [plan/step-16-backend-music.md](plan/step-16-backend-music.md)

### 目标
1. 数据库 3NF 范式化: `music_meta` 独立表, `file_records` 关联 `music_id`
2. AST 查询优化: 索引设计、谓词下推、标准 JOIN
3. 补齐 11 项后端 API（认证/文件/用户）
4. 新增 8 项音乐库/歌单 API（浏览/创建/添加/移除/排序）
5. CORS 跨域支持: 后端全局注入 / Nginx 生产处理

## Step 17 — 前端体验与上传链路优化 ✅ 已正式完成

### 概述

> [plan/step-17-frontend-optimization.md](plan/step-17-frontend-optimization.md) 记录原始功能范围；[plan/bugfix-step17-runtime-regressions.md](plan/bugfix-step17-runtime-regressions.md) 记录 A-F 运行时回归的诊断、修复与最终验收。2026-07-22 的验收结果保留为修复前历史快照：当时用例未覆盖已认证流播 `200/206`，后续复测发现该路径会触发后端重启和 nginx `502`。2026-07-23 已完成 A-F 修复、质量门禁和部署验收，Step 17 正式完成。

### 已落地目标

1. 前端上传改为直接发送原始 `File` 字节，通过 RFC 5987 `filename*` 保留 UTF-8 文件名，并完整呈现后端 JSON、纯文本和 HTML 错误。
2. 前后端统一音频扩展名白名单，拒绝零字节、无效文件名、未知扩展名和超过角色上限的文件；NORMAL 与 VIP 默认单文件上限分别为 `10 MiB` 和 `100 MiB`。
3. 服务端在首个分片落盘前执行九种白名单音频格式的有界签名预检；扩展名与签名不匹配、随机数据和截断头均在落盘前以结构化 `415` 拒绝。该预检不等同于完整解码、恶意媒体检测或内容合规扫描。
4. 上传队列支持稳定标识、受控并发、上传期间继续追加、取消、重试、移除和分项汇总。
5. 修复数字/字符串角色归一化、认证恢复时序、受保护下载与播放、播放器 Blob URL 回收和单实例播放问题。
6. 补齐桌面侧栏、移动抽屉、页面加载/空/错误状态、危险操作确认、表单最小长度、提交锁、焦点样式和移动触控尺寸。
7. 增加部署环境 Playwright 配置，覆盖认证、导航、SPA 深链、上传拦截与追加、响应式布局和浏览器控制台错误；它独立于普通 `scripts/test.sh`。

### 运行时回归修复（2026-07-23）

1. **A：授权流播生命周期**。消除了长期路由处理器引用 `register_routes` 局部闭包导致的悬空引用；已认证完整流播 `200`、Range 流播 `206`、匿名 `401` 与非法 Range `416` 均有自动化响应头和正文断言。
2. **B：chunked 上传内存**。统一普通与 chunked 正文的流式分发，流式/丢弃模式不再累计 `request.body`，终止块会刷新剩余有界缓冲；无有效 `Content-Length` 的上传继续按契约被拒绝。
3. **C：CodeQL 服务探测**。已修复已配置地址不可达时跳过本机服务的问题，探测顺序为已配置地址、`http://localhost:8080`、交互询问地址，并补充脚本回归测试。
4. **D：音频内容签名**。上传在写入分片和数据库前完成有界签名预检，九种允许格式的真实签名才可通过，错误、截断或错配内容不会落盘。
5. **E：下载文件名响应头**。下载响应统一使用安全的 `filename=` 回退名和 RFC 5987 `filename*=`，避免引号、反斜杠、路径分隔符或控制字符造成畸形响应头。
6. **F：空闲 CPU 忙等待**。根因是默认四个 `LockFreeThreadPool` worker 在空队列中以 `std::this_thread::yield()` 反复轮询，且 `epoll_timeout_ms=0` 会使空闲 `epoll_wait` 立即返回并形成热循环。现已改为可由投递和停止唤醒的阻塞等待，并将零超时规范为有界正超时；线程池与 TCP 空闲回归测试覆盖投递、停止、连接处理和唤醒路径。

### 最终验收（2026-07-23）

- `bash scripts/format.sh all`、`bash scripts/lint.sh --all` 和 `bash scripts/compile.sh build` 均以退出码 `0` 完成；clang-tidy、cppcheck 与前端检查均无问题。
- `CODEQL_SUBMIT_TIMEOUT=900 bash scripts/codeql.sh run` 成功完成，任务 `ff62db5b-e741-4d78-ac00-33100da6ce8a` 的 SARIF 结果为 `critical=0`、`high=0`。
- `bash scripts/test.sh` 全部通过：后端 Google Test `42/42`，前端 Vitest `18` 个测试文件、`71` 个用例全部通过。
- Docker 部署、健康检查、日志和容器状态复核通过；真实部署 Playwright 的 `desktop`、`desktop-compact`、`tablet`、`mobile` 四个项目 `4/4` 通过。验收前后后端容器 `RestartCount=0`、`OOMKilled=false`，且没有新增 `502`、`upstream prematurely closed` 或 `connection refused`。

### 历史验收快照（2026-07-22，修复前）

- `bash scripts/lint.sh --changed`：Lint 结果为 `0/0`。
- `bash scripts/compile.sh build`：后端 Release 构建和前端构建均通过。
- `bash scripts/codeql.sh run`：任务 `fa999293-4980-4356-83b3-a2307e87ff18` 完成，`critical=0`、`high=0`。
- `bash scripts/test.sh`：后端 Google Test `41/41` 通过；前端 Vitest `18` 个测试文件、`71` 个用例全部通过。
- Docker 公共入口 `http://127.0.0.1:18080` 健康检查通过。
- 真实部署 Playwright 的 `desktop`、`desktop-compact`、`tablet`、`mobile` 四个项目 `4/4` 通过；最新有效报告为 `frontend/playwright-report/e2e_20260722_214738/index.html`。
- `bash scripts/docker.sh logs --since 5m` 记录到已认证上传返回 HTTP `201`，匿名 stream 请求返回预期的 HTTP `401`。
- 以上为修复前历史快照：E2E 只在开头检查健康状态并只验证匿名 stream `401`，没有覆盖已认证 stream `200/206`。该覆盖缺口随后暴露出授权流播崩溃、chunked 内存风险及其余 A-F 问题；这些问题已在 2026-07-23 修复并重新验收。

以上数量是 2026-07-22 修复前工作树的执行快照，不是永久固定数量，也不能替代本节的最终验收。测试目标和用例继续由源码与测试配置动态发现。

---

## Step 18 — Crystal Music UI 视觉还原 ✅ 已完成

### 概述

> [plan/step-18-ui-visual-restoration.md](plan/step-18-ui-visual-restoration.md) 记录本轮前端 UI 专项实施与 2026-07-27 最终验收。它以 Step 15 的 Crystal Music（翠澄·雨璃）视觉合同为目标，纠正 Step 17 中将大面积青绿玻璃态收敛为中性后台表面的视觉决策；Step 17 已完成的技术修复、运行时可靠性和功能验收均作为不变基线保留。

### 本轮目标

1. 恢复“雨后清晨、青绿澄澈”的 Crystal Music 品牌体验：以浅绿画布、翡翠青绿强调色、半透明层叠表面与毛玻璃效果建立统一视觉语言，并恢复第一期约定的字体、圆角、阴影和层次节奏。
2. 重塑应用壳层、导航、顶栏、内容页、音乐卡片、文件与歌单视图、上传界面以及固定迷你播放器的视觉表达，使其呈现连续的音乐产品体验，而不是带有音乐页面的通用文件管理后台。
3. 保持现有页面信息架构与交互可用性，在桌面、平板和移动视口下让玻璃态、背景、文字对比度、层级和固定区域协调可读。

### 不变边界

1. 本轮只修改前端 UI、视觉令牌、布局呈现和静态展示素材；不改变任何业务功能、用户流程、后端逻辑、REST API、WebSocket、路由、认证、上传协议、音频流播、状态数据模型或数据契约。
2. Step 17 及其运行时回归修复中已完成的上传安全校验、角色与认证恢复、受保护下载与播放、播放器生命周期、响应式导航、无障碍交互、错误状态和测试覆盖必须完整保留，视觉重构不得造成回归。
3. Step 17 关于中性表面与最多 `8px` 圆角的视觉收敛不再作为设计验收依据；其余技术修复和功能行为继续有效。

### 最终验收（2026-07-27）

- `bash scripts/lint.sh --changed` 通过，前端 Oxlint 零告警，本轮无 C++ 变更；`bash scripts/compile.sh build` 后端与前端通过，Fontsource 本地字体进入 `dist/`。
- CodeQL 最终任务 `2e701c4c-70be-4f3e-96fb-b038885c98ec` 为 `critical=0`、`high=0`。
- `bash scripts/test.sh` 通过 Google Test `42/42`、脚本回归和 Vitest `21` 文件 `106/106`，输出无媒体 mock 噪声。
- `bash scripts/docker.sh deploy` 健康，入口为 `http://127.0.0.1:18080`。
- 最终 `npm --prefix frontend run test:e2e` 为 `15 passed`、`3 skipped`、`0 failed`；四个真实部署项目 `4/4`，两个视觉项目的全部适用结构和截图用例通过，浏览器无未处理错误。报告为 `frontend/playwright-report/e2e_20260727_134904/index.html`，结果文件 `frontend/test-results/e2e_20260727_134904/.last-run.json` 为 `passed`。
- 12 张浅色/深色、桌面/小屏截图基线已经人工检查并保留在 `frontend/tests/e2e/visual.spec.ts-snapshots/`。真实浏览器发现并推动修复 Google Fonts 外链和 AudioPlayer Blob URL 回收竞态。

---

## Step 19 — 用户功能闭环、VIP 生命周期与管理员治理 ✅ 已完成

### 概述

> [plan/step-19-user-feature-completion.md](plan/step-19-user-feature-completion.md) 记录角色能力、VIP 生命周期、唯一管理员、资料和治理页面、文件与歌单所有权、播放队列以及隔离 E2E 的完整合同。

### 最终验收（2026-08-01）

1. Step 2 整体审查通过：6 类问题（pending 异常/连接回收、FileDetail/UserPlaylist 竞态、VIP 时间/状态安全、歌单读取去重排序、E2E-03/07/10/11 断言、visual fixture）均经独立审查确认正确实现；E2E-10 新增 `waitForURL` 稳定等待。
2. Step 3 Lint 通过：`bash scripts/lint.sh --changed`，clang-tidy 0/0/0，cppcheck 0/0/0/0，前端 Oxlint 通过。
3. Step 4 编译通过：`bash scripts/compile.sh build`，后端与前端 0 error/warning。
4. Step 5 CodeQL 通过：任务 `d13ae231-d80b-42ac-b180-71df6271de41`，`critical=0`，`high=0`，`executionSuccessful=true`。
5. Step 6 全量测试通过：`bash scripts/test.sh`，Google Test 46 目标全通过，Vitest 34 文件/298 用例全通过，脚本回归全通过。
6. 最终隔离 E2E 通过：`bash scripts/test.sh e2e tests/e2e/user-governance.spec.ts`，13/13，0 failed，隔离环境完整回收。
7. 已提交并 push 到 master。

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

- 正式入口：`bash scripts/pipeline.sh all`。
- 固定顺序：`bash scripts/format.sh all` → `bash scripts/lint.sh --all` → `bash scripts/compile.sh build` → `bash scripts/codeql.sh run` → `bash scripts/test.sh`。
- clang-tidy：`0 error + 0 warning + 0 style`。
- cppcheck `--enable=all`：`0 error + 0 warning + 0 style + 0 performance`。
- CodeQL：`0 critical + 0 high severity`。
- 编译：`0 error + 0 warning`。
- 后端 Google Test 与前端 Vitest：要求全部通过。测试目标与用例从源码动态发现，不在本文维护固定数量。
- Playwright：部署后单独执行，要求配置的桌面与移动视口全部通过，且浏览器控制台无未处理错误。

截至 2026-07-23，Step 17 已完成 A-F 修复及正式验收：最终 CodeQL 任务 `ff62db5b-e741-4d78-ac00-33100da6ce8a` 为 `0 critical + 0 high`，测试为后端 `42/42` 与前端 `18/71`，Docker 四视口 Playwright 为 `4/4`，后端容器 `RestartCount=0`、`OOMKilled=false`。2026-07-22 的分项门禁与 Playwright `4/4` 仍是有效的修复前历史快照，但因未覆盖已认证流播，不能替代 2026-07-23 的最终验收。

截至 2026-07-27，Step 18 已完成视觉还原与正式验收：最终 CodeQL 为 `0 critical + 0 high`，测试为后端 `42/42` 与前端 `21/106`，Docker 六项目 Playwright 为 `15 passed`、`3 skipped`、`0 failed`，12 张视觉基线已经人工检查。

---

## 历史实施进度

> 本节保留各阶段完成时的实施快照，但不维护容易失真的测试目标数、用例数、镜像大小或旧门禁计数；当前验证必须使用上一节列出的稳定脚本。

```
Step 0–2 ─── 基础设施质量优化 (已完成)
  ├─ 当时完成 clang-tidy、cppcheck 与 CodeQL 检查
  ├─ thread-pool: bind → fold lambda, LockFreeQueue 指针安全
  ├─ HTTP Parser: 设计初始化器、认知复杂度拆分、C++20 兼容
  ├─ TcpClient: 非阻塞 + poll 超时 + 粘性缓冲区 + 移动语义 + 测试
  └─ TcpServer: 数组安全、认知复杂度拆分、epoll 封装、[[maybe_unused]]
        │
Step 2 ─── 路由注册 + 请求分发 (已完成)
  ├─ Router: 前缀树(trie)路由，静态段+参数段(:name)，静态优先+回溯
  ├─ HttpServer: 封装 TcpServer+Router，per-conn 局部 parser，keep-alive
  ├─ HttpRequest 扩展 path_params，错误响应(400/404/413/500)，handler 异常捕获
  └─ 测试: test_router + test_http_server 覆盖路由与端到端路径
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
  └─ coroutine: Connection 加 awaitable 支持与对应回归测试
        │
Step 4 ─── file-system 文件读取 + 虚拟路径解析 (IFileSystem 实现) (已完成)
  ├─ IFileSystem 抽象接口 + FileChunk 结构体
  ├─ FileSystem 实现: split_file/compute_file_hash/compute_chunk_hash
  ├─ 存储管理: store_file/read_file/delete_file + 自动建目录
  ├─ 虚拟路径解析: resolve_path + 路径穿越防护
  └─ 测试: test_file_system 覆盖存储、哈希与路径场景
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
  └─ 测试: test_database_pool 覆盖连接池主要场景
        │
Step 7 ─── file-transfer 文件传输模块 (IFileTransfer 实现) ✅ 已完成
  ├─ IFileTransfer 抽象接口: transfer_small / transfer_large / receive_file
  ├─ FileTransfer 实现: 小文件(ITcpClient send_message), 大文件(fork/exec + stdin协议)
  ├─ ChunkHeader 28B 线缆协议 (HPSF magic, 网络字节序, packed)
  ├─ file-send-process 完善: stdin解析, LockFreeThreadPool并发发送, pread分块
  ├─ file-receive-process 实现: raw socket监听, 多线程接收, pwrite重组
  └─ 测试: test_file_transfer 覆盖文件传输主要场景
        │
Step 8 ─── HTTPS/TLS (OpenSSL, 双模式) ✅ 已完成
  ├─ SslContext 封装 + Connection SSL 集成
  ├─ 双模式检测（peek 首字节 0x16 → TLS）
  ├─ 异步 SSL 握手（WANT_READ/WANT_WRITE 重试）
  ├─ 测试证书自动生成（xmake after_build 脚本）
  └─ 测试: test_ssl 覆盖 TLS 主要场景
        │
Step 9 ─── WebSocket 握手 + 帧编解码 ✅ 已完成
  ├─ Base64 编码（RFC 4648）
  ├─ WebSocket 握手（Sec-WebSocket-Accept SHA-1 + Base64 计算）
  ├─ 帧编码（TEXT/BINARY/CLOSE/PING/PONG，服务端不 mask）
  ├─ 帧解码（自动 unmask，7/16/64 位 payload 长度）
  ├─ WsConnection 事件循环（CLOSE/PING/PONG 处理）
  ├─ HttpServer 集成（ws() 路由注册，Upgrade 检测，方案B 事件循环）
  └─ 测试: test_websocket 覆盖握手与帧处理场景
        │
Step 10 ─── LockedThreadPool 有锁通用线程池实现 ✅ 已完成
  ├─ CRTP 继承 ThreadPoolBase<LockedThreadPool>
  ├─ std::queue + std::mutex 有锁任务队列
  ├─ condition_variable 等待通知，wait_for(ms) 超时支持
  ├─ stop() 队列清空 + pending 重置 + jthread 优雅关闭
  ├─ 异常安全：worker 内 try-catch + Logger::_error 记录
  └─ 测试: test_locked_thread_pool 覆盖任务、等待、停止与异常路径
        │
Step 11 ─── main.cpp 整合启动 ✅ 已完成
  ├─ 服务器启动流程: Logger::init → 配置加载 → DatabasePool(MockConnection) → FileSystem → HttpServer → start → 信号等待 → stop → close → shutdown
  ├─ 路由注册: /api/health, /api/users/:id, POST /api/users, /api/users/:id/history, /api/files/:hash, ws://host/ws
  ├─ 配置管理: config.json (nlohmann/json) + 命令行参数 (--port/--threads/--db-host等)
  └─ 测试: 当时完成手动启动验证与全量回归
        │
Step 12 ─── 端到端验证 ✅ 已完成
  ├─ main.cpp 路由注册: POST /api/files/upload + GET /api/files/by-hash/:hash/download
  ├─ B2 修复: CLI 参数覆盖 JSON (parse_json_file → parse_cmd_args 交换)
  ├─ B5 修复: Router path_exists 接口 (405 vs 404 区分)
  ├─ B6/B7 修复: 静态 map 元信息持久化 + 双写 (绕开 MockConnection)
  ├─ 大 body 分片: persistent parser per connection (Connection* key)
  ├─ 线程安全: parsers_ / s_file_meta 加 std::mutex 保护
  ├─ HttpParser 修复: Content-Length:0 直接 COMPLETE
  ├─ 当时使用 verification/verify.sh 验证文件生命周期、并发和边界场景
  └─ verification/ 现已删除；本段仅保留历史记录，不能作为当前验证入口
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
  │   └─ 验证: qps_lock_free_queue + qps_thread_pool 覆盖配置的全部并发级别
  └─ 当时的门禁曾暴露 tcp_client 间歇失败，后续修复端口竞争
  │
  ├─ 遗留修复: SO_REUSEADDR + SO_REUSEPORT 解决 tcp_client 端口竞争
        │
Step 14 ─── 文件上传功能改进 ✅ 已完成
  ├─ 流式分片存储: 2MB 每片，`HttpParser` 流式模式 + chunk_handler 回调
  ├─ 原文件名保留: `Content-Disposition` 提取
  ├─ 认证 & RBAC: GUEST/NORMAL/VIP 三级权限，HMAC-SHA256 Token
  ├─ Bug 修复:
  │   ├─ Bug 1: read_buffer_ 无锁跨线程访问 → malloc 堆损坏（新增 read_mutex_ 保护）
  │   └─ Bug 2: Keep-Alive upload_ctx 复用污染 → 移入 while 循环
  ├─ 当时完成 Lint、测试、编译与 CodeQL 门禁
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
Step 15 ─── 配套前端 Web 界面第一版（历史基线）
  ├─ React 19 + Vite 8 + TypeScript 6 + Tailwind CSS 4 SPA
  ├─ 设计体系: 青绿翡翠 × Glassmorphism × Frosted Glass
  ├─ 页面: 登录/注册/文件列表/文件详情/上传/音乐库/歌单/播放器/用户管理
  ├─ 状态管理: Zustand (auth/player/toast/music)
  ├─ 当时完成 Vite 构建、TypeScript 检查和 Vitest 局部验证
  ├─ 第一版未形成可复现的 Playwright 部署验收闭环
  └─ 历史详情: [plan/step-15-frontend.md](plan/step-15-frontend.md)，后续以 Step 17 为准
        │
Step 16 ─── 后端数据库重构 + 音乐库/歌单 API ✅ 已完成
  ├─ 数据库 3NF 范式化: music_meta + file_records 解耦, users 加 salt
  ├─ AST 查询优化: 索引设计 + 标准 JOIN
  ├─ 补齐 register/me/logout/stream/search/delete/update_user 等后端接口
  ├─ 补齐音乐库浏览、歌单 CRUD 与排序接口
  ├─ 测试修复: validate_token 偏移量 Bug（uid_pos+4 → uid_pos+3）
  └─ 当时完成 Lint、测试与编译门禁
        │
Comprehensive-Test ─── 全方位测试 + 微基准覆盖 ✅ 已完成
  ├─ Bug 修复: Prepared Statement catch 静默吞异常 → 诊断增强（e.what + server_message）
  │             bind API 调整适配 Boost 1.83+，fix-prepared-statement.md 落地
  ├─ Bug 修复: test_tcp_client 间歇性失败 → SO_REUSEADDR + SO_REUSEPORT
  ├─ Bug 修复: main.cpp 拆分可测函数（load_config/add_routes 抽出）
  ├─ 测试增强: 覆盖生命周期、边界、压力、异常、认证、传输和配置路径
  ├─ 微基准增强: logger、认证、TCP、HTTP、内存池、线程池、文件系统等模块
  └─ 当时的门禁结果属于历史快照；当前结果以 scripts/ 正式入口为准
        │
Step 17 ─── 前端体验、上传契约与浏览器验收补强 ✅ 已正式完成
  ├─ 上传: 原始 File body、UTF-8 filename*、前后端策略校验、并发队列与详细错误
  ├─ 认证与媒体: 角色归一化、恢复时序、Bearer 下载/播放、Blob URL 生命周期
  ├─ 交互: 响应式导航、页面状态、确认与忙碌态、表单约束、无障碍补强
  ├─ A-F 修复: 授权流播生命周期、chunked 有界分发、CodeQL 回退探测、签名预检、安全下载名与空闲忙等待均已修复
  ├─ 自动化: 已覆盖已认证 stream `200/206`、匿名 `401`、非法 Range `416`、末尾健康检查和容器重启计数
  ├─ 最终验收: CodeQL `0 critical + 0 high`、Google Test `42/42`、Vitest `18/71`、Docker 四视口 Playwright `4/4`
  └─ 运行状态: 后端容器 `RestartCount=0`、`OOMKilled=false`；2026-07-22 未覆盖授权流播的结果仅保留为历史快照
        │
Step 18 ─── Crystal Music UI 视觉还原 ✅ 已完成
  ├─ 视觉: 浅深主题、青绿玻璃材质、本地 Inter/Righteous 字体与四张原创 WebP 封面
  ├─ 响应式: 375、390、768、1024、1280、1440 六个目标宽度完成结构与截图验收
  ├─ 浏览器修复: 移除 Google Fonts 外链，修复 AudioPlayer Blob URL 回收竞态
  ├─ 最终门禁: CodeQL `0 critical + 0 high`、Google Test `42/42`、Vitest `21/106`
  └─ E2E: 六项目 `15 passed`、`3 skipped`、`0 failed`，12 张截图基线人工检查通过
        │
Step 19 ─── 用户功能闭环、VIP 生命周期与管理员治理 ✅ 已完成（2026-08-01）
  ├─ 四角色/能力模型、UTC VIP 生命周期、唯一 ADMIN bootstrap、Token 实时回查、文件与歌单所有权、QueueEntry 与集中会话清理
  ├─ Step 3-6 正式门禁全部通过：Lint/编译/CodeQL(critical=0,high=0)/全量测试(46 GT+298 Vitest)
  └─ 最终隔离 E2E：13/13 通过，0 failed；已提交并 push 到 master
```

---

## Step 13 — Docker 化部署 ✅ 已完成

### 当前部署形态

1. `Dockerfile` 使用 Ubuntu 24.04 运行时镜像，复制本机 Release 构建产物，并以非 root 用户运行后端。
2. `docker-compose.yml` 编排 MySQL、C++ 后端与 nginx；后端容器内部监听 `9090`，不直接暴露为用户入口。
3. nginx 提供前端静态文件与 `/api/`、`/ws` 反向代理，宿主机默认公共入口为 `http://127.0.0.1:18080`。
4. `http://localhost:8080` 保留给 CodeQL。`scripts/docker.sh` 会拒绝把应用公共入口绑定到 `8080`。
5. 部署密钥和数据库密码由 `.env` 管理，`scripts/docker.sh deploy` 会校验配置、构建前后端、启动服务并等待健康检查。

### 当前入口

- 部署：`bash scripts/docker.sh deploy`
- 状态：`bash scripts/docker.sh status`
- 健康检查：`bash scripts/docker.sh health`
- 完整日志：`bash scripts/docker.sh logs`
- 停止并保留数据卷：`bash scripts/docker.sh stop`

镜像大小和历史门禁数字不作为长期固定目标；每次发布以当前构建产物、健康检查和正式流水线结果为准。

---

## 模块完成度总览

| 模块 | 当前能力 | 状态 |
|------|----------|------|
| core | 配置、依赖装配、路由注册、生命周期与上传策略 | 已实现 |
| logger | 两阶段受控单例 | 已实现 |
| net/http | 路由、HTTP 解析、Range、认证、上传与响应处理 | 已实现 |
| net/tcp | 客户端、服务端、epoll ET、SSL 与优雅关闭 | 已实现 |
| net/coroutine | Connection awaitable 支持 | 已实现 |
| net/thread-pool | CRTP、LockFree 与 Locked 双实现 | 已实现 |
| memory-pool | 分级内存池与静态 deleter | 已实现 |
| net/file-transfer | 小文件、大文件与分片传输进程 | 已实现 |
| file-system | 文件切分、哈希、存储与路径防护 | 已实现 |
| db | boost::mysql 连接池、认证数据与音乐数据访问 | 已实现 |
| net/ssl | TLS 上下文、连接集成与双模式检测 | 已实现 |
| net/websocket | 握手、帧编解码与连接事件循环 | 已实现 |
| frontend | 认证、文件、上传、音乐库、歌单、播放器和响应式交互 | 已实现；Step 17 技术基线与 Step 18 Crystal Music 视觉还原均已完成正式验收 |
| tests | Google Test、Vitest 与部署后 Playwright | 2026-08-01 Step 19 最终验收：Google Test 46 目标全通过，Vitest 34 文件/298 用例全通过，隔离 E2E 13/13；数量动态发现 |

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

| 范围 | 主要测试文件或目录 | 重点场景 |
|------|--------------------|----------|
| TCP 客户端与服务端 | `tests/test_tcp_*.cpp` | 连接、动态端口、并发、断连、大流量和启停 |
| HTTP 与路由 | `tests/test_http_*.cpp`, `tests/test_router.cpp` | 解析、请求/响应、管线、Keep-Alive、路由和压力路径 |
| Range 与 URL | `tests/test_range_parser*.cpp`, `tests/test_url_decode*.cpp` | 边界区间、越界和异常编码 |
| 内存池与线程池 | `tests/test_memory_pool*.cpp`, `tests/test_*thread_pool*.cpp`, `tests/test_lock_free_queue.cpp` | 并发、碎片、停止、异常和压力路径 |
| 协程与函数包装 | `tests/test_coroutine*.cpp`, `tests/test_move_only_function.cpp` | 生命周期、异常、移动语义和批量任务 |
| 文件系统与传输 | `tests/test_file_system*.cpp`, `tests/test_file_transfer*.cpp` | 存储、哈希、并发、分片与断连 |
| 数据库与认证 | `tests/test_database_pool*.cpp`, `tests/test_boost_mysql_connection.cpp`, `tests/test_auth_service.cpp` | 连接池、字段转换、认证、超时和错误路径 |
| TLS 与 WebSocket | `tests/test_ssl*.cpp`, `tests/test_websocket*.cpp` | 上下文、握手、帧、掩码和异常输入 |
| 配置与业务 API | `tests/test_config.cpp`, `tests/test_step16_api.cpp` | 配置加载、音乐库和歌单接口 |
| 上传策略 | `tests/test_upload_policy.cpp`, `tests/test_http_server.cpp` | 扩展名、大小、文件名、分片前拒绝与上传上下文 |
| 前端单元与组件 | `frontend/tests/**/*.test.*` | API 契约、状态管理、上传队列、页面和组件交互 |
| 部署浏览器验收 | `frontend/tests/e2e/` | 认证、导航、SPA 深链、上传、响应式布局与控制台错误 |

后端测试目标由顶层 `xmake.lua` 根据 `tests/test_*.cpp` 动态生成，前端测试由 Vitest 与 Playwright 按各自配置发现。本文不维护固定用例数；是否通过以当前工作树的脚本原始输出为准。

## 运行与端口约定

- 构建后端与前端：`bash scripts/compile.sh build`
- 全部单元和集成测试：`bash scripts/test.sh`
- 正式质量流水线：`bash scripts/pipeline.sh all`
- Docker 部署与健康检查：`bash scripts/docker.sh deploy`、`bash scripts/docker.sh health`
- 应用公共入口：`http://127.0.0.1:18080`
- CodeQL 默认探测地址：`http://localhost:8080`；也可通过 `CODEQL_SERVER_URL` 指定其他主机

已删除的 `verification/` 和 `scripts/run_tp_debug.sh` 只可能出现在明确标注的历史记录中，不能作为当前命令。部署环境用户流程使用 `bash scripts/test.sh e2e tests/e2e/user-governance.spec.ts`，GDB 调试使用 `xmake run -d <target>`。
