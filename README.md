# High-Performance Server

基于 epoll ET（边缘触发）+ 线程池 + C++20 协程的高性能 TCP/HTTP 服务器框架，支持 HTTPS、WebSocket、Range 流式传输、文件传输、数据库连接池。

## 技术栈

| 类别 | 选择 | 版本 | 说明 |
|------|------|------|------|
| 语言 | C++ | C++20 | jthread、stop_token、span、coroutine |
| 编译器 | g++ | 14.x | Linux 原生编译 |
| 构建系统 | xmake | 3.0.9+ | Lua 脚本化构建 |
| 网络模型 | epoll ET | - | 边缘触发 + eventfd 跨线程唤醒 |
| 测试框架 | Google Test | 1.17 | 单元测试 |
| TLS | OpenSSL | 3.x | 双模式检测（明文/TLS 自动识别）|
| 数据库 | boost::mysql | - | 连接池 + 预处理查询 |
| 静态分析 | clang-tidy + cppcheck | - | 代码风格 + 深度检查 |
| 语义分析 | CodeQL | Docker | 安全漏洞 + 质量门禁 |

## 快速开始

### 前置条件

```bash
# 1. 安装 xmake
curl -fsSL https://xmake.io/shget.text | bash
# 确保 ~/.local/bin 在 PATH 中
export PATH="$HOME/.local/bin:$PATH"

# 2. 安装系统依赖（Ubuntu 22.04）
sudo apt install -y build-essential libssl-dev libstdc++-11-dev

# 3. 安装项目依赖（gtest, nlohmann_json 自动下载）
xmake require
```

### 编译

```bash
# Debug 模式（默认，含 AddressSanitizer + UndefinedBehaviorSanitizer）
xmake

# Release 模式（-O2 优化）
xmake f -m release -y && xmake

# 重新配置并编译
xmake f -c -y && xmake

# 多核编译
xmake -j$(nproc)
```

### 运行

```bash
# 使用默认配置（config.json, 端口 9090）
xmake run

# 或直接运行二进制
./bin/high-performance-server

# 指定端口
./bin/high-performance-server --port 9090

# 指定配置文件
./bin/high-performance-server --config /path/to/config.json
```

### 停止服务器

按 `Ctrl-C` 发送 SIGINT 信号，服务器会优雅关闭：

```
^C
[2026-07-05 12:00:00] [WARN] [00000] tcp_server.cpp:57 收到信号 2，正在关闭服务器...
[2026-07-05 12:00:00] [INFO] [00000] tcp_server.cpp:237 TcpServer 已停止
[2026-07-05 12:00:00] [INFO] [00000] main.cpp:348 正在关闭数据库连接池...
[2026-07-05 12:00:00] [INFO] [00000] main.cpp:351 服务器已停止
```

**信号处理机制**：`TcpServer::init()` 通过 `sigaction` 注册 SIGINT/SIGTERM 处理函数。收到信号时，静态方法 `TcpServer::signal_handler` 调用 `s_instance_->stop()`，设置 `running_ = false` 并通过 eventfd 唤醒 epoll_wait，使事件循环自然退出。

## 命令行选项

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--port <port>` | uint16 | 8080 | 监听端口（0 = 内核自动分配）|
| `--config <path>` | string | config.json | 配置文件路径 |
| `--threads <n>` | size_t | 4 | 工作线程数 |
| `--db-host <host>` | string | 127.0.0.1 | 数据库主机 |
| `--db-port <port>` | uint16 | 3306 | 数据库端口 |
| `--data-dir <dir>` | string | ./data | 数据存储目录 |
| `--ssl-cert <path>` | string | - | SSL 证书路径（同时启用 SSL）|
| `--ssl-key <path>` | string | - | SSL 密钥路径（同时启用 SSL）|
| `--ssl-ca <path>` | string | - | SSL CA 证书路径 |
| `--ssl-verify` | flag | false | 启用客户端证书验证 |
| `--help` | flag | - | 显示帮助信息 |

```bash
# 完整示例
./high-performance-server \
  --port 443 \
  --threads 8 \
  --ssl-cert ./build/certs/cert.pem \
  --ssl-key ./build/certs/key.pem \
  --db-host 10.0.0.1 \
  --data-dir /mnt/data
```

## 配置文件

服务器启动时读取 `config.json`，命令行参数优先级高于配置文件（JSON 先加载，CLI 后覆盖）。

### 完整示例

```json
{
  "server": {
    "port": 9090,
    "backlog": 128,
    "thread_count": 4,
    "epoll_timeout_ms": 100
  },
  "database": {
    "host": "127.0.0.1",
    "port": 3306,
    "username": "root",
    "password": "",
    "database": "music_server",
    "pool_size": 10,
    "connect_timeout_ms": 3000,
    "read_timeout_ms": 5000
  },
  "ssl": {
    "enabled": false,
    "cert_file": "./build/certs/cert.pem",
    "key_file": "./build/certs/key.pem",
    "ca_file": "",
    "verify_peer": false
  },
  "filesystem": {
    "root_dir": "./data"
  }
}
```

### 字段说明

#### server 节

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `port` | uint16 | 9000 | 监听端口 |
| `backlog` | size_t | 128 | listen 队列长度 |
| `thread_count` | size_t | 4 | LockFreeThreadPool 工作线程数 |
| `epoll_timeout_ms` | int | 100 | epoll_wait 超时（毫秒）|

#### database 节

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `host` | string | 127.0.0.1 | 数据库主机 |
| `port` | uint16 | 3306 | 数据库端口 |
| `username` | string | root | 用户名 |
| `password` | string | "" | 密码 |
| `database` | string | music_server | 数据库名 |
| `pool_size` | size_t | 10 | 连接池大小 |
| `connect_timeout_ms` | uint32 | 3000 | 连接超时 |
| `read_timeout_ms` | uint32 | 5000 | 读取超时 |

#### ssl 节

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `enabled` | bool | false | 是否启用 TLS |
| `cert_file` | string | ./build/cert.pem | 服务器证书路径 |
| `key_file` | string | ./build/key.pem | 私钥路径 |
| `ca_file` | string | ./build/ca.pem | CA 证书路径 |
| `verify_peer` | bool | false | 是否验证客户端证书 |

#### filesystem 节

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `root_dir` | string | ./data | 文件存储根目录 |

## 脚本工具

项目提供以下脚本，均从项目根目录执行：

### `bash scripts/<脚本名>` — 独立脚本工具

所有脚本支持无参交互菜单模式和 `-h/--help` 参数：

| 脚本 | 子命令 | 说明 |
|------|--------|------|
| `compile.sh` | `build`, `--clean` | 编译项目（多核）|
| `format.sh` | `all`, `<路径...>` | clang-format 格式化 `.cpp/.hpp/.h`|
| `codeql.sh` | `run` | 提交 CodeQL 分析（自动探测服务器地址）|
| `pipeline.sh` | `all`, `format`, `lint`, `compile`, `test` | 一键流水线 |
| `benchmark.sh` | `micro`, `load`, `diff`, `gen-data`, `build` | 性能基准测试 |
| `docker.sh` | `build`, `image`, `run`, `stop`, `up`, `down`, `all` | Docker 部署 |

### `bash scripts/lint.sh [选项] [文件/目录...]` — Lint 检查

同时运行 clang-tidy 和 cppcheck，自动生成 `compile_commands.json`（若不存在或 xmake.lua 更新）。

| 选项 | 说明 |
|------|------|
| `--changed` | 仅检查 Git 已变更文件（相对 HEAD，含未跟踪）|
| `-j, --jobs N` | 并发数（默认 CPU 核数）|
| `-h, --help` | 显示帮助 |
| `文件/目录...` | 指定检查范围（非 `--changed` 时）|

示例：

```bash
# 全量检查
bash scripts/lint.sh

# 增量检查
bash scripts/lint.sh --changed

# 并发 8 线程
bash scripts/lint.sh -j 8

# 检查指定目录
bash scripts/lint.sh net/http/ tests/

# 检查指定文件
bash scripts/lint.sh net/http/src/http_parser.cpp
```

### `bash scripts/test.sh [测试名]` — 测试运行

| 参数 | 说明 |
|------|------|
| 无参数 | 运行全部测试（20 个测试二进制）|
| `测试名` | 仅运行指定测试文件（如 `test_tcp_server`）|

### `bash scripts/docker.sh <子命令>` — Docker 部署

子命令：

| 子命令 | 说明 |
|--------|------|
| `build` | Release 编译 |
| `image` | 构建 Docker 镜像 `hps-server` |
| `run` | 运行容器（端口 9090）+ 健康检查 |
| `stop` | 停止并删除容器 |
| `up` | `docker compose up -d`（含 MySQL）|
| `down` | `docker compose down` |
| `all` | 依次执行 build → image → stop → run |

无参数时进入交互菜单。

### `bash verification/verify.sh` — 端到端验证

一键执行 15 个维度（V1~V15）、37 项验证，覆盖编译、单元测试、REST API、错误处理、Keep-Alive、WebSocket、信号停止、SSL/TLS、CLI 参数、文件上传/下载/哈希、并发连接、边界条件。输出 PASS/FAIL 报告。

详细验证步骤见 `verification/README.md`。

### `bash setup.sh` — 环境初始化

在 Ubuntu 22.04 上安装：g++/gdb/make、xmake、clang-tidy/cppcheck/clang-format。

```bash
bash setup.sh
```

## 外部接口

### REST API

基于 HTTP/1.1，请求体为 JSON，响应体为 JSON 或二进制流。路径参数以 `:name` 格式声明，匹配时自动注入 `req.path_params`。

#### 端点

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/health` | 健康检查 |
| GET | `/api/users/:id` | 获取用户信息 |
| POST | `/api/users` | 创建用户 |
| GET | `/api/users/:id/history` | 获取用户下载历史 |
| GET | `/api/files/:hash` | 获取文件元信息 |
| POST | `/api/files/upload` | 上传文件（body 即文件内容） |
| GET | `/api/files/:hash/download` | 下载文件（支持 Range） |

#### 请求 / 响应格式

**GET /api/health**
```json
// → 200
{"status":"ok","uptime":42}
```

**GET /api/users/:id**
```json
// ← {"id": 1}
// → 200 {"user_id":1,"username":"alice"}
// → 404 {"error":"user not found"}
```

**POST /api/users**
```json
// → {"username":"alice","email":"alice@example.com"}
// ← 201 {"status":"created"}
// ← 500 {"error":"create failed"}
```

**GET /api/users/:id/history**
```json
// ← {"id": 1}
// → 200 {"downloads":[{"log_id":1,"file_hash":"abc123"}]}
// → 400 {"error":"missing id"}
```

**GET /api/files/:hash**
```json
// ← {"hash": "sha256hex"}
// → 200 {"file_hash":"abc","file_path":"uploads/abc","file_size":1024}
// → 404 {"error":"file not found"}
```

**POST /api/files/upload**
```
// → body: 原始二进制（文件内容）
// ← 201 {"hash":"sha256hex","size":1024}
// ← 200 {"hash":"sha256hex","size":1024,"exists":true}
// ← 400 {"error":"empty body"}
// ← 500 {"error":"store failed"}
```

**GET /api/files/:hash/download**
```
// → 200 application/octet-stream（完整文件）
// → 206 Partial Content（Range 请求）
// → 404 {"error":"file not found"}
```

#### 统一错误响应

```
// HTTP 400 / 404 / 500
{"error":"描述信息"}
```

### WebSocket

#### 握手

```
GET /ws HTTP/1.1
Host: server
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13
```

成功 → `101 Switching Protocols`

#### 帧格式（RFC 6455）

每个数据帧包含 opcode（操作类型）和 payload（载荷）：

| Opcode | 名称 | 说明 |
|--------|------|------|
| `0x0` | CONTINUATION | 分片续帧 |
| `0x1` | TEXT | UTF-8 文本 |
| `0x2` | BINARY | 二进制 |
| `0x8` | CLOSE | 关闭连接 |
| `0x9` | PING | 心跳 |
| `0xA` | PONG | 心跳回复 |

关闭帧附带 2 字节大端关闭码：

| 码值 | 含义 |
|------|------|
| 1000 | 正常关闭 |
| 1001 | 端点离开 |
| 1002 | 协议错误 |
| 1003 | 不支持的数据类型 |
| 1007 | 无效 payload |
| 1008 | 策略违规 |
| 1009 | 消息过大 |

服务端通过 `ws_encode_frame(opcode, payload)` 编码发送；
客户端数据通过 `ws_decode_frame(data)` 解码为 `WsFrame{fin, opcode, payload}`。

### Range 流式传输

支持 HTTP Range 请求头，用于部分下载和断点续传。

#### 请求

```
GET /api/files/:hash/download HTTP/1.1
Range: bytes=0-1023
```

Range 格式：

| 格式 | 示例 | 含义 |
|------|------|------|
| `bytes=start-end` | `bytes=0-1023` | 指定区间 |
| `bytes=start-` | `bytes=1024-` | 从指定位置到末尾 |
| `bytes=-suffix` | `bytes=-1024` | 最后 N 字节 |
| 多区间 | `bytes=0-99,200-299` | 多个区间 |

#### 响应

| 情况 | 状态码 | Content-Type |
|------|--------|-------------|
| 单区间 | 206 | `application/octet-stream` |
| 多区间 | 206 | `multipart/byteranges; boundary=HPS_<random>` |
| 无效区间 | 416 | `application/json` |

单区间响应头：
```
Content-Range: bytes 0-1023/1048576
```

多区间响应体：
```
--HPS_abc123
Content-Type: application/octet-stream
Content-Range: bytes 0-99/1000

[数据]
--HPS_abc123
Content-Type: application/octet-stream
Content-Range: bytes 200-299/1000

[数据]
--HPS_abc123--
```

### 文件传输（进程间协议）

大文件传输 fork 出 `file-send-process` 独立进程，通过管道传递以下元数据（纯文本）：

```
第一行: <total_size> <path> <peer_ip> <peer_port> <chunk_count>
后续行: <chunk_index> <offset> <size>
```

示例：
```
1048576 /data/video.mp4 10.0.0.2 9001 4
0 0 262144
1 262144 262144
2 524288 262144
3 786432 262144
```

| 字段 | 类型 | 说明 |
|------|------|------|
| total_size | uint64 | 文件总字节数 |
| path | string | 源文件路径 |
| peer_ip | string | 目标 IP |
| peer_port | uint16 | 目标端口 |
| chunk_count | uint32 | 分片数 |
| chunk_index | uint32 | 分片序号 |
| offset | uint64 | 分片在文件中的偏移 |
| size | uint64 | 分片字节数 |

#### ChunkHeader（线路二进制协议）

每片数据前附加 28 字节固定头（所有多字节字段为大端序）：

```
偏移  大小  字段         说明
0     4    magic        魔数 "HPSF" (0x48505346)
4     4    chunk_index  分片序号
8     8    offset       文件偏移
16    8    chunk_size   分片数据大小
24    4    total_chunks 总分片数
```

## 测试指南

### 运行测试

```bash
# 全部测试（推荐）
bash scripts/test.sh

# 全部测试（xmake 直接调用）
xmake test

# 单个测试文件
bash scripts/test.sh test_tcp_server
xmake test -f test_tcp_server

# 单个用例
xmake run test_tcp_server --gtest_filter="*SignalStopServer*"
```

### 端到端验证

```bash
bash verification/verify.sh
```

一键执行 15 维度、37 项验证，输出 PASS/FAIL 报告（详见 `verification/README.md`）。

### 测试覆盖

| 模块 | 测试数 | 覆盖内容 |
|------|--------|----------|
| TcpServer | ~8 | 初始化/启动/停止/信号停止/echo/并发/无Handler/断连 |
| TcpServer Connection | 10 | 构造/读/写/关闭/状态/大缓冲/空操作 |
| HttpParser | 19 | GET/POST/chunked/分片feed/过大/重置/错误 |
| HttpRequest | 4 | 默认状态/clear/方法转换 |
| HttpResponse | 8 | 状态/header/serialize/clear/大小写忽略 |
| UrlDecode | 8 | 普通/plus/百分号/混合/非法/空/大小写 |
| Router | 9 | 注册/匹配/参数/404/405/冲突/通配 |
| HttpServer | 7 | 端到端请求/响应/keep-alive/错误码 |
| MemoryPool | - | 分配/释放/碎片 |
| ThreadPool (LockFree) | - | 无锁队列/任务调度 |
| ThreadPool (Locked) | 8 | 有锁队列/超时/优雅停止 |
| Coroutine | 5 | 异常/完成/await_read/write |
| FileSystem | 14 | 分片/hash/存储/删除/路径遍历防护 |
| DatabasePool | 14 | 连接池/CRUD/超时/健康检查 |
| FileTransfer | 7 | 小文件/ChunkHeader/接收重组 |
| SSL/TLS | 9 | SslContext/握手/加密通信/双模式 |
| WebSocket | 12 | 握手/帧编解码/Base64/集成 |
| Range Parser | 10 | 单区间/多区间/非法/边界 |
| **合计** | **~167** | |
| **性能基准测试** | **12 个二进制** | 12 个模块微基准测试 + HTTP 负载测试（详见 benchmark/README.md）|

## 信号处理与优雅停止

服务器通过以下机制确保优雅关闭：

```
用户按 Ctrl-C (SIGINT) 或 kill 发送 SIGTERM
        │
        ▼
sigaction 触发 TcpServer::signal_handler(int sig)
        │
        ▼
s_instance_->stop()
  ├─ running_ = false
  └─ eventfd 写入 1 字节（epoll_wait 立即返回）
        │
        ▼
event_loop 检测到 running_ == false，退出循环
        │
        ▼
cleanup_resources()
  ├─ 关闭所有客户端连接
  ├─ 关闭 eventfd / epoll_fd / server socket
  └─ signal(SIGINT, SIG_DFL), signal(SIGTERM, SIG_DFL)
        │
        ▼
main() 继续执行：
  ├─ 关闭数据库连接池
  └─ Logger::shutdown()
```

关键点：
- `TcpServer::init()` 通过 `sigaction()` 注册信号处理（非 `std::signal()`，避免竞态）
- `epoll_wait` 超时 100ms 确保及时响应停止信号
- 所有资源通过 RAII + 显式 `close` 双重保障
- 线程池 `jthread` 自动 join

## 开发工作流

```bash
# 1. 格式化代码
bash scripts/format.sh

# 2. Lint 检查（clang-tidy + cppcheck）
bash scripts/lint.sh
# 增量检查（仅 Git 变更文件）
bash scripts/lint.sh --changed

# 3. 编译
bash scripts/compile.sh

# 4. 运行测试
bash scripts/test.sh

# 5. 性能基准测试
bash scripts/benchmark.sh micro    # 微基准测试
bash scripts/benchmark.sh load    # HTTP 负载测试
bash scripts/benchmark.sh diff    # 基线对比

# 6. CodeQL 分析
bash scripts/codeql.sh

# 7. 全流程
bash scripts/pipeline.sh
```

### 性能基准测试详细说明

包含 12 个模块的微基准测试和 HTTP 负载测试：

```bash
# 编译 + 运行微基准测试（Release 模式）
bash scripts/benchmark.sh micro

# 生成测试数据（1KB ~ 100MB）
bash scripts/benchmark.sh gen-data

# HTTP 负载测试（需服务器运行）
bash scripts/benchmark.sh load

# 微基准测试清单见 benchmark/README.md
```

## Docker 部署

### 前置条件

- Docker Engine 24+
- 已编译的 release 二进制（`xmake f -m release -y && xmake`）

### 一键构建运行

```bash
bash scripts/docker.sh
```

### 手动构建镜像

```bash
docker build -t hps-server .
```

### 运行容器

```bash
# 基本运行
docker run -d --name hps-server -p 9090:9090 hps-server

# 挂载配置文件和 SSL 证书
docker run -d --name hps-server -p 9090:9090 \
  -v ./config.json:/app/config.json:ro \
  -v ./build/certs:/app/build/certs:ro \
  -v ./data:/app/data \
  hps-server
```

### docker-compose 编排（含 MySQL）

```bash
docker compose up -d
```

### 验证运行

```bash
curl http://localhost:9090/api/health
# {"status":"ok","uptime":42}
```

### 停止

```bash
docker stop hps-server            # 单容器
docker compose down               # 编排停止
```

## 质量门禁

| 检查项 | 标准 | 命令 |
|--------|------|------|
| 编译 | 0 error + 0 warning | `xmake` |
| clang-tidy | 0 error + 0 warning + 0 style | `bash scripts/lint.sh` |
| cppcheck | 0 error + 0 warning + 0 style + 0 performance | `bash scripts/lint.sh` |
| CodeQL | 0 critical + 0 high | `bash scripts/codeql.sh` |
| 测试 | 100% 通过 | `bash scripts/test.sh` |

## 项目结构

```
project/
├── AGENTS.md              # AI 辅助开发指南
├── xmake.lua              # 顶层构建配置
├── README.md              # 本文件
├── goal.md                # 项目目标文档
├── Dockerfile             # Docker 运行时镜像（从本机 COPY 产物）
├── config.json            # 服务器配置
├── core/                  # 主程序入口
│   └── src/main.cpp
├── logger/                # 日志模块（两阶段单例）
├── memory-pool/           # 内存池（CRTP 静态多态）
├── file-system/           # 文件系统
├── db/                    # 数据库连接池（boost::mysql）
├── net/                   # 网络层
│   ├── coroutine/         # 协程（C++20 std::coroutine）
│   ├── thread-pool/       # 线程池（LockFree + Locked 双实现）
│   ├── tcp/
│   │   ├── tcp_client/    # TCP 客户端
│   │   └── tcp_server/    # TCP 服务器（epoll ET + SSL + 优雅关闭）
│   ├── http/              # HTTP 协议实现
│   ├── ssl/               # OpenSSL 封装
│   ├── websocket/         # WebSocket 帧编解码
│   ├── file-transfer/     # 文件传输（小文件单连接 + 大文件多进程）
│   ├── file-send-process/ # 文件发送独立进程
│   └── file-receive-process/ # 文件接收独立进程
├── tests/                 # 单元测试（~167 用例）
├── scripts/               # 运维脚本
│   ├── compile.sh         # 编译
│   ├── format.sh          # 格式化
│   ├── codeql.sh          # CodeQL 安全分析
│   ├── pipeline.sh        # 一键流水线
│   ├── lint.sh            # 静态检查（clang-tidy + cppcheck）
│   ├── test.sh            # 测试
│   ├── benchmark.sh       # 性能基准测试
│   ├── docker.sh          # Docker 部署
│   ├── docker.sh          # Docker 部署（子命令：build/image/run/stop/up/down/all）
│   ├── lint.sh            # Lint 检查（clang-tidy + cppcheck，支持 --changed/-j/路径）
│   └── test.sh            # 测试运行器（支持指定测试名）
├── verification/          # 端到端验证
│   ├── verify.sh          # 15 维度 37 项功能验证
│   ├── README.md          # 验证步骤说明
│   └── ws_test.py         # WebSocket 测试
├── setup.sh               # Ubuntu 环境初始化（g++/xmake/clang-tidy）
├── plan/                  # 开发计划
└── lib/                   # 动态库输出目录
```

## 编码规范

### 命名规约

| 类别 | 风格 | 示例 |
|------|------|------|
| 类/类型 | PascalCase | `ThreadPool`, `TcpServer`, `HttpParser` |
| 函数/变量 | snake_case | `init_pool`, `connection_count`, `read_from_fd` |
| 成员变量 | snake_case + `_` 后缀 | `config_`, `running_`, `fd_` |
| 常量 | `k` + PascalCase | `kMaxEpollEvents`, `kMaxBodySize` |
| 枚举值 | UPPER_SNAKE_CASE | `REQUEST_LINE`, `HEADERS`, `BODY` |
| 全局变量 | `g_` 前缀 | `g_start_time` |
| 命名空间 | snake_case | `hps` |

### 头文件

- `#pragma once` 代替宏保护
- 包含顺序：本模块 → 标准库 → 系统头文件
- 优先前置声明

### 内存管理

- RAII 优先（unique_ptr, shared_ptr, jthread）
- 禁止裸 new/delete
- 网络 fd 在析构或 close_connection 中释放

### C++20 特性

- `std::jthread` + `std::stop_token`：自动 join
- `std::span` / `std::string_view`：零拷贝视图
- `std::atomic`：无锁状态标志
- `std::coroutine`：协程实现异步 IO
- `std::format`（若可用）：类型安全格式化

## License

MIT
