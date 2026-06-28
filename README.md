# High-Performance Server

高性能 TCP 服务器框架，基于 epoll ET 模式 + 线程池 + 协程的设计方案，支持 HTTP 协议解析，面向高并发场景。

## 技术栈

| 类别 | 选择 | 版本 | 说明 |
|------|------|------|------|
| 语言 | C++ | C++20 | 使用 std::jthread、std::stop_token、std::span 等现代特性 |
| 编译器 | g++ | 14.x | Linux 原生编译 |
| 构建系统 | xmake | 3.0.9+ | 跨平台 Lua 脚本化构建 |
| 测试框架 | Google Test | 1.17 | 单元测试 + 断言 |
| 调试器 | GDB | - | 运行时调试 |
| 静态分析 | clang-tidy | - | 代码风格 + 静态检查 |
| 静态分析 | cppcheck | - | 深度静态分析 |
| 语义分析 | CodeQL | Docker | 安全漏洞 + 质量门禁 |

## 模块架构

```
core/               ← 主入口（main.cpp）
  │
  ├── logger/       ← 日志模块（同步/异步日志，级别过滤）
  │
  ├── memory-pool/  ← 内存池（对象池分配器）
  │
  └── net/          ← 网络层
        ├── coroutine/       ← 协程基础设施（coroitem.hpp）
        ├── thread-pool/     ← 线程池（无锁任务队列 + jthread）
        ├── tcp/
        │   ├── ctcpclient/  ← TCP 客户端封装
        │   └── ctcpserver/  ← TCP 服务器（epoll + ThreadPool）
        ├── http/            ← HTTP 协议实现
        │   ├── include/
        │   │   ├── case_insensitive.h  ← 大小写不敏感 HeaderMap
        │   │   ├── http_request.h      ← HttpRequest + HttpMethod
        │   │   ├── http_response.h     ← HttpResponse + serialize
        │   │   ├── http_parser.h       ← 流式解析器状态机
        │   │   └── url_decode.h        ← URL 百分比解码
        │   └── src/ ← 实现文件
        ├── file-send-process/    ← 文件发送服务
        └── file-receive-process/ ← 文件接收服务
```

### 核心模块说明

| 模块 | 说明 |
|------|------|
| **CTcpServer** | 基于 epoll ET 模式的高性能 TCP 服务器。主线程事件循环，业务处理委托给 ThreadPool，支持 eventfd 跨线程唤醒、SIGINT/SIGTERM 优雅关闭、可配置参数 |
| **ThreadPool** | 基于 std::jthread 和 LockFreeQueue（无锁环形缓冲）的线程池。支持任务投递、优雅停止（stop → request_stop → join） |
| **Connection** | TCP 连接封装，非阻塞 I/O + 读写缓冲区，状态管理，超时追踪 |
| **HttpParser** | 流式 HTTP 请求解析器状态机，支持 Content-Length 和 Chunked Transfer-Encoding，分片 feed，100 MiB body 限制 |
| **HttpRequest/HttpResponse** | HTTP 请求/响应数据结构，含大小写不敏感 HeaderMap |
| **MemoryPool** | 对象池分配器，减少高频对象的动态内存分配开销 |

## 构建指引

### 前置条件

```bash
# 安装 xmake
curl -fsSL https://xmake.io/shget.text | bash

# 安装依赖（项目会自动处理 gtest）
xmake require
```

### 常用命令

| 命令 | 说明 |
|------|------|
| `xmake` | 编译项目（debug 模式） |
| `xmake f -c && xmake` | 清除配置后重新编译 |
| `xmake run` | 运行默认目标 |
| `xmake run <target>` | 运行指定目标 |
| `xmake test` | 运行所有测试 |
| `xmake test -f <case>` | 运行指定测试用例 |
| `xmake -b <target>` | 构建指定目标 |
| `xmake clean` | 清理构建产物 |
| `xmake project -k compile_commands` | 生成 compile_commands.json |
| `xmake -v` | 详细编译输出 |

### 编译模式

- **release**：`xmake f -m release && xmake`（默认，-O3 优化）
- **debug**：`xmake f -m debug && xmake`（-g -O0，含 AddressSanitizer）

## 测试指引

### 运行全部测试

```bash
xmake test
```

### 运行单个测试

```bash
xmake run test_test_ctcpserver
xmake run test_test_http_parser
```

### 直接执行测试二进制

```bash
LD_LIBRARY_PATH=lib ./build/linux/x86_64/release/test_test_http_parser
```

### 测试覆盖

| 测试文件 | 测试数 | 覆盖模块 |
|----------|--------|----------|
| `test_ctcpserver.cpp` | 7 | CTcpServer 初始化/启动/停止/echo/并发/无Handler/断连 |
| `test_ctcpserver_connection.cpp` | 10 | Connection 构造/读/写/关闭/状态/大缓冲/空操作 |
| `test_http_request.cpp` | 4 | HttpRequest 默认状态/clear/方法转换 |
| `test_http_response.cpp` | 8 | HttpResponse 状态/header/serialize/clear/大小写忽略 |
| `test_http_parser.cpp` | 19 | 解析器 GET/POST/chunked/分片feed/过大/重置/错误 |
| `test_url_decode.cpp` | 8 | URL 解码 普通/plus/百分号/混合/非法/空/大小写 |
| **合计** | **56** | |

## 质量门禁

本项目采用严格的质量门禁策略，所有代码必须通过以下检查：

### 1. 编译（0 error + 0 warning）

```bash
xmake
```

### 2. 静态分析

```bash
# clang-tidy（0 error + 0 warning + 0 style）
clang-tidy net/http/src/*.cpp -- -std=c++20 -Inet/http/include -Ilogger/include

# cppcheck（0 error + 0 warning + 0 style + 0 performance）
cppcheck --enable=all --inconclusive --suppress=missingIncludeSystem \
  --language=c++ --std=c++20 -Inet/http/include -Ilogger/include \
  net/http/src/*.cpp
```

### 3. CodeQL（0 critical + 0 high）

CodeQL 分析通过 Docker 容器运行，端口 8080：

```bash
xmake project -k compile_commands
# 预处理 compile_commands.json（过滤外部依赖，directory 改为 ".")
# 打包源码（src/、tests/、xmake.lua，排除 *.o、.xmake/、build/）
# 发送分析请求
curl -X POST "http://<server-ip>:8080/analyze" \
  -F "source=@source.tar.gz;type=application/gzip" \
  -F "compile_commands=@compile_commands_fixed.json;type=application/json"
```

### 4. 测试（100% 通过）

```bash
xmake test
# 或逐个运行
for t in build/linux/x86_64/release/test_test_*; do
  LD_LIBRARY_PATH=lib timeout 30 "$t" || echo "FAIL: $t"
done
```

### 门禁流程

```
编译通过 → clang-tidy → cppcheck → CodeQL → 测试通过 → 通过
                        ↓ 失败              ↓ 失败
                        修复代码 ← ← ← ← ← ←
```

## 目录结构

```
project/
├── AGENTS.md              # AI 辅助开发指南
├── xmake.lua              # 顶层构建配置
├── README.md              # 本文件
├── goal.md                # 项目目标文档
├── opencode.jsonc         # opencode 配置
├── tui.json               # TUI 配置
├── plan/                  # 开发计划
├── scripts/               # 工具脚本
├── core/                  # 主程序入口
│   └── src/main.cpp
├── logger/                # 日志模块
│   ├── include/
│   └── src/
├── memory-pool/           # 内存池
│   ├── include/
│   └── src/
├── file-system/           # 文件系统
│   ├── include/
│   └── src/
├── net/                   # 网络层
│   ├── coroutine/         # 协程基础
│   ├── thread-pool/       # 线程池
│   │   ├── include/
│   │   └── src/
│   ├── tcp/
│   │   ├── ctcpclient/    # TCP 客户端
│   │   └── ctcpserver/    # TCP 服务器
│   │       ├── include/   #   ctcpserver.h, connection.h
│   │       └── src/       #   ctcpserver.cpp, connection.cpp
│   ├── http/              # HTTP 协议
│   │   ├── include/       #   case_insensitive.h, http_request.h,
│   │   │                  #   http_response.h, http_parser.h, url_decode.h
│   │   └── src/           #   *.cpp
│   ├── file-send-process/
│   └── file-receive-process/
├── tests/                 # 单元测试
│   ├── test_ctcpserver.cpp
│   ├── test_ctcpserver_connection.cpp
│   ├── test_http_request.cpp
│   ├── test_http_response.cpp
│   ├── test_http_parser.cpp
│   └── test_url_decode.cpp
├── lib/                   # 构建输出（共享库）
└── build/                 # 构建缓存
```

## 编码规范

### 命名规约

| 类别 | 风格 | 示例 |
|------|------|------|
| 类名 | PascalCase | `ThreadPool`, `CTcpServer`, `HttpParser` |
| 函数/变量 | snake_case | `init_pool`, `connection_count`, `read_from_fd` |
| 成员变量 | snake_case + 下划线 | `config_`, `running_`, `fd_`, `state_` |
| 常量/枚举值 | UPPER_SNAKE_CASE | `MAX_THREADS`, `MAX_BODY_SIZE`, `PAYLOAD_TOO_LARGE` |
| 命名空间 | snake_case | `hps` |

### 头文件

- 使用 `#pragma once` 代替宏保护
- 优先前置声明，减少头文件依赖
- 头文件包含顺序：本模块头文件 → 标准库 → 系统头文件

### 内存与资源管理

- 优先 RAII（std::unique_ptr、std::shared_ptr、std::jthread 等）
- 禁止裸 new/delete
- fd 等系统资源在析构函数或 close_connection 中释放

### C++20 特性使用

- `std::jthread` + `std::stop_token`：自动 join 的线程管理
- `std::span` / `std::string_view`：零拷贝的缓冲区引用
- `std::atomic`：无锁状态标志
- `std::chrono::steady_clock`：稳定的时间度量

## 依赖关系

```
test_* （二进制）
  ├── ctcpserver （共享库）
  │   ├── connection （共享库——与 ctcpserver 同目标）
  │   ├── thread-pool （共享库）
  │   │   └── lock_free_queue （头文件）
  │   ├── logger （共享库）
  │   └── memory-pool （共享库）
  ├── http （共享库）
  └── gtest （外部包，静态链接）
```

测试二进制通过 RPATH（`--disable-new-dtags`）定位共享库，确保运行时能找到 `lib/` 目录下的动态库及其传递依赖。

## License

MIT
