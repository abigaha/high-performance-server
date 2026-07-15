# 性能基准测试

## 概述

本目录包含项目的全部性能基准测试，分为两部分：

1. **微基准测试**（`bench_*.cpp`）— 使用 Google Benchmark 对各模块核心操作进行微基准测试
2. **负载测试** — 使用 wrk 对 HTTP 服务器进行端到端负载测试

## 运行方式

### 一键运行全部微基准测试

```bash
bash scripts/benchmark.sh micro
```

### 一键运行负载测试

```bash
bash scripts/benchmark.sh load
```

### 仅编译 benchmark 二进制

```bash
bash scripts/benchmark.sh build
```

### 生成测试数据

```bash
bash scripts/benchmark.sh gen-data
```

### 查看基线对比

```bash
bash scripts/benchmark.sh diff micro
bash scripts/benchmark.sh diff load
```

### 通过 benchmark.sh 调用

```bash
# 交互菜单（无参数）
bash scripts/benchmark.sh

# 或直接指定子命令
bash scripts/benchmark.sh micro
bash scripts/benchmark.sh load
```

## 微基准测试清单

| 文件 | 覆盖模块 | 测试维度 |
|------|---------|---------|
| `bench_http_parser.cpp` | HTTP 解析器 | GET 短路径/长路径、POST 小 body/大 body、重置复用、多请求头 |
| `bench_router.cpp` | 路由器（前缀树） | 静态匹配、参数匹配、404、path_exists、长路径、大路由表(5000条) |
| `bench_thread_pool.cpp` | 线程池（LockFree + Locked） | 轻量任务(100/1k/10k)、重量任务 |
| `bench_memory_pool.cpp` | 内存池 + malloc 对比 | 分配释放(32B~4KB)、批量分配、混合大小、malloc 对照 |
| `bench_websocket.cpp` | WebSocket 帧编解码 | TEXT 编码、解码、BINARY 编码、mask 解码 |
| `bench_file_system.cpp` | 文件系统 | SHA-256(1KB~1MB)、split_file(1MB/10MB)、store+read |
| `bench_move_only_function.cpp` | MoveOnlyFunction | 构造(空/捕获/大对象)、调用、移动构造、std::function 对比 |
| `bench_lock_free_queue.cpp` | 无锁队列 | SPSC(1k/10k ops) |
| `bench_database_pool.cpp` | 数据库连接池 | get_user、create_user、get_file_meta、store_file_meta、get_history |
| `bench_ssl.cpp` | SSL 上下文 | 创建、未启用模式 |
| `bench_chunk_header.cpp` | 文件传输 ChunkHeader | to_network、from_network、序列化 |
| `bench_coroutine.cpp` | 协程 | 创建+运行开销 |

## 负载测试场景

- **端点**: `/api/health` GET, `/api/users/:id` GET
- **并发梯度**: 1/10/50/100/500/1000
- **连接模型**: keep-alive, 短连接
- **Payload**: 1KB/1MB/10MB/20MB/.../100MB（生成在 `data/bench/`）
- **TLS**: 明文 vs SSL 分开测试

## 性能基线

结果自动保存到 `benchmark/baseline/`，JSON 格式包含：

- 时间戳、Git 提交哈希、机器信息（CPU/内存）
- 各 benchmark 的均值/中位数/标准差
- 负载测试的 RPS、p50/p90/p99 延迟
