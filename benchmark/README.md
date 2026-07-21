# 性能基准测试

## 测试分层

项目提供三类性能测试：

1. `bench_*.cpp`：基于 Google Benchmark 的单进程微基准。
2. `qps_*.cpp`：模块级并发阶梯测试，覆盖核心容器、网络、HTTP、文件、认证等模块。
3. `rps`：通过 `wrk` 访问已经部署的同源 HTTP 入口，覆盖 Nginx、后端、数据库和文件系统的端到端链路。

统一入口为：

```bash
bash scripts/benchmark.sh --help
```

## 依赖

- 构建工具：`xmake`
- 微基准依赖：Google Benchmark
- QPS 控制：`timeout`
- RPS 工具：`wrk`、`curl`、`python3`
- RPS 服务：目标入口必须已经部署完成，数据库表已初始化，上传目录可写

可在不连接服务的情况下检查本机依赖和 QPS 目标发现：

```bash
bash scripts/benchmark.sh check
```

## 微基准

运行全部 `bin/bench_*`：

```bash
bash scripts/benchmark.sh micro
```

仅编译 benchmark：

```bash
bash scripts/benchmark.sh build
bash scripts/benchmark.sh build --debug
```

可通过 `BENCH_FLAGS` 覆盖 Google Benchmark 参数：

```bash
BENCH_FLAGS="--benchmark_min_time=0.5s" bash scripts/benchmark.sh micro
```

## 模块 QPS

脚本从 `benchmark/qps_*.cpp` 实时生成期望目标名。编译后只要存在缺失目标或 `bin/qps_*` 陈旧目标，就会拒绝运行，避免报告漏模块或混入已删除模块。

快速验证会运行所有目标，每个场景使用并发 1、4，各持续 1 秒：

```bash
QPS_PROFILE=smoke bash scripts/benchmark.sh qps
```

完整阶梯会运行所有目标，并发为 1、4、16、64、256、512、1024：

```bash
QPS_PROFILE=full bash scripts/benchmark.sh qps
```

也可将 profile 作为位置参数：

```bash
bash scripts/benchmark.sh qps smoke
bash scripts/benchmark.sh qps full
```

每个 QPS 目标都由独立超时保护。某个目标失败或超时后，脚本继续运行其余目标，最终统一返回非零并列出全部失败项。默认 smoke 单目标超时 120 秒，full 单目标超时 900 秒，可覆盖：

```bash
QPS_TIMEOUT_SECONDS=300 QPS_PROFILE=full bash scripts/benchmark.sh qps
```

## 端到端 RPS

`rps` 不编译、不启动后端，只压测 `RPS_BASE_URL` 指定的已部署同源入口。该变量必须是无路径、查询和片段的 HTTP(S) 入口；Docker 默认应用入口为 `http://127.0.0.1:18080`：

```bash
bash scripts/benchmark.sh rps smoke
RPS_BASE_URL=http://127.0.0.1:18080 bash scripts/benchmark.sh rps full
```

宿主机 `8080` 保留给 CodeQL 服务。若已有 `.env` 通过 `HPS_HTTP_PORT` 修改了 Docker 应用端口，须将 `RPS_BASE_URL` 改为相同的入口，例如 `RPS_BASE_URL=http://127.0.0.1:19080`。部署会在构建前检查该端口是否已被占用，端口不可用时会直接失败。

兼容入口 `load` 与 `rps` 行为相同：

```bash
bash scripts/benchmark.sh load smoke
```

### 预检

正式压测前会执行以下步骤，任一步失败都会生成失败报告并返回非零：

1. 验证 `/api/health` 返回 200。
2. 注册本次运行专用的唯一用户，并从注册响应取得 Token。
3. 预上传 1KB、1MB 随机文件，保存 `file_id` 和 `file_hash`。
4. 使用预上传结果构造下载及 Range 场景，不依赖数据库中的历史数据。

### 场景

| 类别 | 场景 | 期望状态 |
|---|---|---|
| 读取 | `GET /api/health` | 200 |
| 读取 | `GET /api/auth/me` | 200 |
| 读取 | `GET /api/files?offset=0&limit=20` | 200 |
| 读取 | `GET /api/music/library?offset=0&limit=20` | 200 |
| 上传 | `POST /api/files/upload`，1KB | 201 |
| 上传 | `POST /api/files/upload`，1MB | 201 |
| 下载 | `GET /api/files/by-hash/:hash/download`，1KB | 200 |
| 下载 | `GET /api/files/by-hash/:hash/download`，1MB | 200 |
| Range | `GET /api/files/:id/stream`，`bytes=0-1023` | 206 |

### Profile

| Profile | 读取并发 | 下载/Range 并发 | 上传并发 | 读取/下载时长 | 上传时长/请求间隔 | 单元数 | 错误策略 |
|---|---|---|---|---:|---:|---:|---|
| `smoke` | 1/10 | 1/5 | 1 | 5 秒 | 2 秒/250ms | 16 | 严格门禁 |
| `full` | 1/10/50/100/500/1000 | 1/10/50/100 | 1/2 | 20 秒 | 5 秒/250ms | 40 | 严格门禁 |
| `overload` | 2000/5000/10000 | 100/500/1000 | 2/5 | 20 秒 | 5 秒/250ms | 25 | 记录过载错误 |

`smoke` 和 `full` 中任何非 2xx、非预期状态码、连接/读/写/超时错误、`wrk` 非零退出或指标缺失，都会使命令整体返回非零。`overload` 将 HTTP/Socket 错误标记为 `overloaded`，用于观察容量拐点；工具失败、指标缺失或矩阵缺项仍返回非零。

上传场景会在不改变 1KB/1MB 载荷长度的前提下，为每个线程的每次请求写入唯一标记。上传只接受 201，因此服务端按重复哈希返回的 200 快路径会被判为非预期状态。由于普通压测用户没有删除权限，默认上传矩阵使用独立低并发、短时长和 250ms 请求间隔控制持久化数据量；反复执行前应重置测试环境的数据卷。
可覆盖矩阵参数：

```bash
RPS_PROFILE=full \
RPS_DURATION_SECONDS=30 \
RPS_REPEATS=2 \
RPS_READ_CONCURRENCY="1 10 50 100" \
RPS_TRANSFER_CONCURRENCY="1 10 25" \
RPS_UPLOAD_CONCURRENCY="1 5 10" \
RPS_UPLOAD_DURATION_SECONDS=5 \
RPS_UPLOAD_DELAY_MILLISECONDS=250 \
RPS_REQUEST_TIMEOUT_SECONDS=10 \
RPS_BASE_URL=https://example.test \
bash scripts/benchmark.sh rps
```

负载机与服务同机运行时会共享 CPU、内存和网络栈，报告只能用于同机回归比较。容量结论应使用独立负载机，并保持服务配置、数据库数据量和网络条件一致。

## 报告

每次运行使用同一时间戳生成报告，不创建无时间戳别名：

```text
benchmark/reports/
├── micro_YYYYMMDD_HHMMSS.txt
├── micro_YYYYMMDD_HHMMSS.json
├── micro_YYYYMMDD_HHMMSS_raw/
├── qps_YYYYMMDD_HHMMSS.txt
├── qps_YYYYMMDD_HHMMSS.json
├── qps_YYYYMMDD_HHMMSS_raw/
├── rps_YYYYMMDD_HHMMSS.txt
├── rps_YYYYMMDD_HHMMSS.json
└── rps_YYYYMMDD_HHMMSS_raw/
```

RPS JSON/TXT 汇总包含：

- Git 提交与 dirty 状态、主机、CPU、内存、profile、目标入口；
- 每个矩阵单元的 RPS、平均延迟、p50/p90/p99；
- HTTP 状态码分布、non-2xx、非预期状态码；
- `wrk` 请求总数与跨线程响应总数对账、connect/read/write/timeout 错误和原始输出路径；
- 期望单元数、实际完成数和整体结论。

`raw/` 目录保留每个目标或矩阵单元的完整原始输出，便于复核汇总结果。

对比最近两个同类型时间戳 JSON 报告：

```bash
bash scripts/benchmark.sh diff micro
bash scripts/benchmark.sh diff qps
bash scripts/benchmark.sh diff rps
bash scripts/benchmark.sh diff load
```

## 测试数据

生成 1KB 至 100MB 的通用 benchmark 数据文件：

```bash
bash scripts/benchmark.sh gen-data
```

RPS 使用每次运行临时生成的 1KB、1MB 随机载荷，结束后自动清理本地临时文件；服务端数据按部署环境的持久化策略保留。
