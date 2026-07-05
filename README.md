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
# Debug 模式（默认，含 AddressSanitizer）
xmake

# Release 模式（-O3 优化）
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
  --ssl-cert ./build/cert.pem \
  --ssl-key ./build/key.pem \
  --db-host 10.0.0.1 \
  --data-dir /mnt/data
```

## 配置文件

服务器启动时读取 `config.json`，命令行参数优先级高于配置文件（JSON 先加载，CLI 后覆盖）。

### 完整示例

```json
{
  "server": {
    "port": 9000,
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
    "cert_file": "./build/cert.pem",
    "key_file": "./build/key.pem",
    "ca_file": "./build/ca.pem",
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

## 路由与 API

服务器启动后自动注册以下路由：

### REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/health` | 健康检查（返回 uptime） |
| GET | `/api/users/:id` | 获取用户信息 |
| POST | `/api/users` | 创建用户 |
| GET | `/api/users/:id/history` | 获取用户下载历史 |
| GET | `/api/files/:hash` | 获取文件元信息 |
| POST | `/api/files/upload` | 上传文件 |
| GET | `/api/files/:hash/download` | 下载文件 |

### WebSocket

| 路径 | 说明 |
|------|------|
| `/ws` | WebSocket 连接端点 |

## 测试指南

### 运行全部测试

```bash
xmake test
```

### 运行单个测试文件

```bash
xmake test -f test_tcp_server
# 或
./build/linux/x86_64/release/test_test_tcp_server
```

### 运行单个用例

```bash
xmake run test_test_tcp_server --gtest_filter="*SignalStopServer*"
```

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
bash scripts/dev.sh format

# 2. Lint 检查（clang-tidy + cppcheck）
bash scripts/dev.sh lint
# 增量检查（仅 Git 变更文件）
bash scripts/lint.sh --changed

# 3. 编译
bash scripts/dev.sh compile

# 4. 运行测试
bash scripts/dev.sh test

# 5. CodeQL 分析
bash scripts/dev.sh codeql

# 6. 全流程
bash scripts/dev.sh all
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
| CodeQL | 0 critical + 0 high | `bash scripts/dev.sh codeql` |
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
├── scripts/               # 开发脚本
│   ├── dev.sh             # 开发工具菜单
│   ├── lint.sh            # clang-tidy + cppcheck
│   └── test.sh            # 测试运行器
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
