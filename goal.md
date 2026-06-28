# 音乐软件 HTTP 服务器 — 项目目标与架构规划

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

## 模块优先级与依赖关系

```
                    ┌──────────────────────────────────┐
                    │       core/main.cpp              │
                    │  (服务器启动/配置/注册路由)        │
                    └──────────────┬───────────────────┘
                                   │
                    ┌──────────────▼───────────────────┐
                    │         HttpServer                │
                    │  (路由/HTTPS/WS/中间件管道)       │
                    └──┬──────────┬──────────┬─────────┘
                       │          │          │
                ┌──────▼──┐ ┌─────▼────┐ ┌──▼─────────┐
                │TCP Server│ │FileSystem│ │ WebSocket   │
                │ctcpserver│ │(音乐文件) │ │(实时通信)   │
                └────┬─────┘ └──────────┘ └────────────┘
                     │
           ┌─────────┼─────────┬──────────┐
      ┌────▼───┐ ┌──▼────┐ ┌──▼────┐ ┌──▼──────┐
      │coroutine│ │thread │ │logger │ │memory   │
      │CoroItem │ │-pool  │ │       │ │-pool    │
      └────────┘ └───────┘ └───────┘ └─────────┘

      ┌──────────┐ ┌──────────┐
      │  MySQL   │ │ OpenSSL   │
      │ 连接池   │ │ (TLS)    │
      └──────────┘ └──────────┘
```

### P0 — 最高优先 (基础设施)

| 模块 | 状态 | 说明 |
|------|------|------|
| **ctcpserver → HttpServer** | 🔶 进行中 | HTTP 协议解析(✅)、请求/响应类型(✅)、URL 解码(✅)；路由+分发(🔴) |
| **CTcpClient** | ✅ 就绪 | TCP 客户端，支持超时/重连/移动语义/大小写不敏感 HeaderMap |
| **thread-pool** | ✅ 就绪 | 并发请求处理 + LockFreeQueue |
| **coroutine (CoroItem\<T\>)** | ✅ 就绪 | 异步非阻塞 I/O，每个连接一个协程 |
| **WebSocket** | 🔴 待实现 | 基于 TCP Upgrade，实时音乐状态同步/评论 |

关键文件位置：`net/http/`、`net/websocket/`

### P1 — 业务支撑层

| 模块 | 状态 | 说明 |
|------|------|------|
| **file-system** | 🔴 空壳 | 音乐文件管理（保存/读取/删除/路径解析）、Range 随机读取支持 |
| **MySQL 连接池** | 🔴 待集成 | 管理音乐元数据、用户数据、歌单 |

关键文件位置：`file-system/src/`、`db/`

### P2 — 增强功能

| 模块 | 说明 |
|------|------|
| **HTTPS/TLS** | OpenSSL/mbedTLS，可选启用 |
| **Range 流式传输** | HTTP Range 请求处理，支持音乐播放进度拖动 |

### P3 — 业务模块

| 模块 | 说明 |
|------|------|
| **file-send-process** | 音乐文件发送 |
| **file-receive-process** | 音乐文件上传 |

---

## 续写进度

```
Step 0–2 ─── 基础设施质量优化 (已完成)
  ├─ clang-tidy 0/0/0 + cppcheck 0 + CodeQL 0/0
  ├─ thread-pool: bind → fold lambda, LockFreeQueue 指针安全
  ├─ HTTP Parser: 设计初始化器、认知复杂度拆分、C++20 兼容
  ├─ CTcpClient: 非阻塞 + poll 超时 + 粘性缓冲区 + 移动语义 + 测试
  └─ ctcpserver: 数组安全、认知复杂度拆分、epoll 封装、[[maybe_unused]]
        │
Step 2 ─── 路由注册 + 请求分发 (当前关卡)
        │
Step 3 ─── file-system 文件读取 + 虚拟路径解析
        │
Step 4 ─── Range 流式传输 (解析、206 Partial Content)
        │
Step 5 ─── MySQL 连接池 + SongRepo/UserRepo
        │
Step 6 ─── HTTPS/TLS (OpenSSL, 双模式)
        │
Step 7 ─── WebSocket 握手 + 帧编解码
        │
Step 8 ─── file-send/receive + main.cpp 整合
        ▼
      9. **main.cpp 整合启动**
```

## 续写建议顺序（实际编码）

1. ~~ctcpserver 重构 + HTTP 请求/响应基本类型~~ (`net/http/`) ✅
   - `CTcpServer` 纳入 `hps` namespace
   - `HttpRequest`/`HttpResponse` 数据结构
   - 头部解析、URL 解码、body 管理
   - `CTcpClient` 非阻塞超时 + 缓冲区 + 移动语义

2. **路由注册 + 请求分发** (`net/http/router.hpp`) ← 下一步
   - 前缀树 (trie) 或正则路由
   - `server.get("/song/:id", handler)` 风格

3. **file-system 文件读取** (`file-system/src/`)
   - 音乐文件管理（读/写/删/查）
   - 虚拟路径 → 物理路径解析

4. **Range 流式传输** (依赖 1+3)
   - 解析 `Range: bytes=start-end`
   - 返回 `206 Partial Content` + `Content-Range`

5. **MySQL 连接池 + 数据模型** (`db/`)
   - 基于 lock_free_queue 的连接池
   - SongRepo、UserRepo 等

6. **HTTPS/TLS** (依赖 1)
   - OpenSSL 封装
   - 可选启用（HTTP/HTTPS 双模式）

7. **WebSocket** (依赖 1)
   - 握手处理 (101 Switching Protocols)
   - 帧编解码

8. **file-send/receive** (依赖 3)
   - 内部进程文件流转流程

9. **main.cpp 整合启动**
   - 服务器启动流程
   - 路由注册
   - 配置管理

---

## 关键设计决策

| 领域 | 决策 |
|------|------|
| HTTP 解析 | 手写轻量解析器 或 llhttp |
| 路由匹配 | 前缀树 (trie) |
| 连接模型 | 每个连接一个 Coroutine |
| 并发处理 | thread-pool + Coroutine 协作 |
| 数据库 | MySQL Connector/C++ 或 libmysqlclient |
| TLS | OpenSSL |
| 命名空间 | 全部统一到 `hps` |
| 命名规则 | 成员变量 `_` 后缀，全局函数/变量 `_` 前缀 |
