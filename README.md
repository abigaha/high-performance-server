# High-Performance Server

这是一个基于 C++20 的高性能文件与音乐服务。后端使用 epoll ET、线程池和协程处理 TCP/HTTP 连接，提供认证、文件分片存储、下载与 Range 流播、音乐库、歌单、WebSocket、TLS 和 MySQL 数据访问；前端使用 React、TypeScript 和 Vite。

## 系统组成

生产部署的请求链路如下：

```text
浏览器
  -> nginx（默认 http://127.0.0.1:18080）
     -> / 与前端静态资源：frontend/dist/
     -> /api/*：C++ 后端容器的 9090 端口
     -> /ws：C++ 后端 WebSocket
        -> MySQL：用户、文件元数据、音乐和歌单
        -> data/：文件分片
```

本地开发时可以不启动 nginx，直接访问 C++ 后端。仓库中的 `config.json` 将后端端口设置为 `9090`。

主要技术栈：

| 类别 | 技术 |
|---|---|
| 后端 | C++20、g++、xmake v3 |
| 网络 | epoll ET、eventfd、C++20 协程、线程池 |
| 协议 | HTTP/1.1、HTTPS、WebSocket、Range |
| 数据 | boost::mysql、MySQL 8、分片文件系统 |
| 前端 | React 19、TypeScript 6、Vite 8、Tailwind CSS 4、Zustand |
| 测试 | Google Test、Vitest、Playwright、Google Benchmark、wrk |
| 质量 | clang-format、clang-tidy、cppcheck、CodeQL |
| 部署 | Docker Compose、nginx |

## 项目结构

```text
.
├── core/                       # 主程序、配置解析和认证服务
├── db/                         # MySQL 连接池、模型和初始化 schema
├── file-system/                # 文件分片存储
├── logger/                     # 日志模块
├── memory-pool/                # 分层内存池
├── net/
│   ├── coroutine/              # C++20 协程
│   ├── file-transfer/          # 文件传输协议
│   ├── http/                   # HTTP 解析、路由和服务器
│   ├── ssl/                    # OpenSSL 封装
│   ├── tcp/                    # TCP 客户端和服务端
│   ├── thread-pool/            # 无锁与有锁线程池
│   └── websocket/              # WebSocket 帧与连接
├── frontend/                   # React 前端
├── tests/                      # xmake 动态发现的 Google Test 源文件
├── benchmark/                  # 微基准、模块 QPS 和 RPS 说明
├── scripts/                    # 构建、质量、部署和压测脚本
├── deploy/nginx.conf           # nginx 反向代理配置
├── db/schema.sql               # MySQL 初始化脚本
├── Dockerfile
├── docker-compose.yml
├── config.json                 # 本地运行配置
├── setup.sh                    # Ubuntu/Debian 开发环境初始化
└── xmake.lua                   # 顶层构建及动态测试/基准目标
```

`plan/` 和 `goal.md` 记录规划、历史实施过程与当前阶段状态；了解运行行为时应以源码、脚本帮助和本 README 为准。

## 首次使用

### 1. 初始化开发环境

在 Ubuntu 或 Debian 上执行：

```bash
bash setup.sh
```

脚本可以重复运行，会自动选择 root 或 `sudo`，安装 C++ 工具链、Git、curl、OpenSSL/Boost 开发包、Node.js/npm、Python 3、tar、bc、clang-format、clang-tidy 和 cppcheck。前端要求 Node.js 20.19+ 或 22.12+；版本不满足时脚本安装 Node.js 22。未检测到 xmake 时会通过官方安装器安装，最后执行 `xmake require` 获取 xmake 包依赖。

以下能力按需另行安装，`setup.sh` 只提示、不自动安装：

- Docker Engine 与 Docker Compose 插件：容器化部署。
- `wrk`：端到端 RPS 压测。
- `libbenchmark-dev`：Google Benchmark 微基准。
- MySQL 8 服务：不使用 Docker 时的本地后端运行。

### 2. 准备数据库和认证密钥

本地运行需要可访问的 MySQL，并初始化表结构：

```bash
mysql -u root -p < db/schema.sql
export AUTH_SECRET="$(openssl rand -hex 48)"
```

根据实际数据库修改 `config.json`，或使用环境变量覆盖。认证密钥必须来自 `AUTH_SECRET` 或 `config.json` 的 `server.auth_secret`，两者都为空时服务器会拒绝启动。

### 3. 编译

```bash
bash scripts/compile.sh build
```

该命令构建全部 C++ 目标以及前端生产包。主要产物为：

- `bin/high-performance-server`：主服务。
- `bin/qps_*`、`bin/bench_*`：已满足依赖条件时生成的性能目标。
- `lib/`：项目动态库。
- `frontend/dist/`：前端静态文件。

需要清除 xmake 配置缓存并重新编译时执行：

```bash
bash scripts/compile.sh --clean
```

### 4. 运行后端

```bash
export AUTH_SECRET="$(openssl rand -hex 48)"
xmake run high-performance-server
```

也可以直接运行：

```bash
./bin/high-performance-server --config config.json
```

健康检查：

```bash
curl http://127.0.0.1:9090/api/health
```

按 `Ctrl-C` 或发送 `SIGTERM` 后，服务器会唤醒事件循环，停止接收连接并关闭数据库、线程和日志资源。

### 5. 运行前端开发服务器

另开终端执行：

```bash
cd frontend
npm ci
VITE_API_URL=http://127.0.0.1:9090 npm run dev
```

Vite 当前没有开发代理。前后端分开运行时必须将 `VITE_API_URL` 指向后端；经 nginx 同源部署时保持该变量为空。更多说明见 `frontend/README.md`。

## 配置

配置覆盖顺序由低到高为：

```text
代码内置默认值 < JSON 配置文件 < 环境变量 < 命令行参数
```

默认读取根目录 `config.json`，可通过 `--config <path>` 选择其他文件。程序仍保留内置回退端口 `8080`，但仓库配置将后端端口覆盖为 `9090`；项目部署不应使用 `8080`，该宿主机端口保留给 CodeQL。

### 环境变量

| 环境变量 | 作用 |
|---|---|
| `DB_HOST` | MySQL 主机 |
| `DB_PORT` | MySQL 端口 |
| `DB_USER` | MySQL 用户 |
| `DB_PASSWORD` | MySQL 密码 |
| `DB_NAME` | MySQL 数据库名 |
| `SERVER_PORT` | 后端监听端口 |
| `AUTH_SECRET` | 认证令牌签名密钥，优先于 JSON |

### 命令行参数

| 参数 | 说明 |
|---|---|
| `--config <path>` | JSON 配置路径，默认 `config.json` |
| `--port <port>` | 监听端口，`0` 表示由内核分配 |
| `--threads <n>` | 工作线程数 |
| `--db-host <host>` | MySQL 主机 |
| `--db-port <port>` | MySQL 端口 |
| `--data-dir <dir>` | 文件数据根目录 |
| `--ssl-cert <path>` | 证书路径并启用 TLS |
| `--ssl-key <path>` | 私钥路径并启用 TLS |
| `--ssl-ca <path>` | CA 证书路径 |
| `--ssl-verify` | 验证客户端证书 |
| `--help` | 显示程序帮助 |

### JSON 配置

```json
{
  "server": {
    "port": 9090,
    "backlog": 128,
    "thread_count": 4,
    "epoll_timeout_ms": 100,
    "auth_secret": "",
    "normal_max_size": 10485760,
    "vip_max_size": 104857600
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

`normal_max_size` 和 `vip_max_size` 分别限制普通用户和 VIP 用户的单文件上传大小，仓库默认值为 `10 MiB` 和 `100 MiB`。上传请求必须携带可解析的 `Content-Length`；服务端会在分片落盘前按角色检查该长度，缺失或无效时直接拒绝。敏感值应通过部署环境注入，不应提交真实密钥和生产密码。

## HTTP 与 WebSocket 接口

除公开接口外，请在请求中携带登录或注册返回的令牌：

```http
Authorization: Bearer <token>
```

角色从低到高为 `GUEST`、`NORMAL`、`VIP`。当前 `core/src/main.cpp` 注册的接口如下：

| 方法 | 路径 | 权限 | 说明 |
|---|---|---|---|
| `POST` | `/api/auth/register` | 公开 | 注册并返回令牌 |
| `POST` | `/api/auth/login` | 公开 | 登录并返回令牌 |
| `POST` | `/api/auth/logout` | 公开 | 客户端登出确认 |
| `GET` | `/api/auth/me` | 登录 | 当前用户信息 |
| `GET` | `/api/health` | 公开 | 服务状态与运行时间 |
| `GET` | `/api/files` | NORMAL | 文件分页与类型筛选 |
| `GET` | `/api/files/search` | NORMAL | 文件搜索与排序 |
| `GET` | `/api/files/:id` | NORMAL | 文件元数据 |
| `GET` | `/api/files/:id/download` | NORMAL | 按文件 ID 下载 |
| `GET` | `/api/files/:id/stream` | NORMAL | 文件流播，支持单区间 Range |
| `DELETE` | `/api/files/:id` | VIP | 删除文件及关联元数据 |
| `POST` | `/api/files/upload` | NORMAL | 上传允许的音频原始文件体 |
| `GET` | `/api/files/by-hash/:hash/download` | NORMAL | 按哈希下载完整文件 |
| `GET` | `/api/users/:id` | 公开 | 查询基本用户信息 |
| `PUT` | `/api/users/:id` | 本人登录 | 修改邮箱或密码 |
| `GET` | `/api/music/library` | NORMAL | 搜索音乐库 |
| `GET` | `/api/music/library/:id` | NORMAL | 音乐详情及关联文件 |
| `GET` | `/api/users/:id/playlists` | 登录 | 用户歌单列表 |
| `POST` | `/api/users/:id/playlists` | 本人登录 | 创建歌单 |
| `GET` | `/api/playlists/:id/items` | NORMAL | 歌单项列表 |
| `POST` | `/api/playlists/:id/items` | NORMAL | 添加音乐到歌单 |
| `DELETE` | `/api/playlists/:id/items/:music_id` | NORMAL | 从歌单移除音乐 |
| `PUT` | `/api/playlists/:id/items/reorder` | NORMAL | 按 `music_ids` 重排歌单 |
| WebSocket | `/ws` | 公开 | WebSocket 连接与帧处理 |

注册示例：

```bash
curl -X POST http://127.0.0.1:9090/api/auth/register \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"secret123","email":"alice@example.com"}'
```

### 文件上传约束

文件上传的请求体是单个文件的原始二进制内容，不是 `multipart/form-data`。文件名从 `Content-Disposition` 读取；后端同时支持普通 `filename` 和 RFC 5987 UTF-8 `filename*`：

```bash
curl -X POST http://127.0.0.1:9090/api/files/upload \
  -H "Authorization: Bearer ${TOKEN}" \
  -H 'Content-Type: audio/mpeg' \
  -H "Content-Disposition: attachment; filename*=UTF-8''sample.mp3" \
  --data-binary @sample.mp3
```

允许的扩展名为 `.mp3`、`.ogg`、`.wav`、`.flac`、`.aac`、`.m4a`、`.wma`、`.ape`、`.opus`，匹配时不区分大小写。服务端拒绝未知扩展名、无效文件名、零字节文件和超过角色上限的文件，并返回带 `error` 与稳定 `code` 的 JSON；校验失败时不会启动哈希、分片写入或数据库 handler。当前类型边界是扩展名白名单，不等同于完整音频解码或内容安全扫描。

前端在选择或拖放时同步执行扩展名、浏览器 MIME 冲突、零字节和角色大小预检，用稳定 ID 管理队列，并将并发限制为 2。上传期间可以继续安全追加文件，支持逐项取消、重试、移除和部分成功汇总。客户端预检用于及时反馈，不能替代后端最终校验；后端的 JSON、纯文本或网关错误会保留原始可读原因。

Range 流播示例：

```bash
curl http://127.0.0.1:9090/api/files/1/stream \
  -H "Authorization: Bearer ${TOKEN}" \
  -H 'Range: bytes=0-1023' \
  --output part.bin
```

当前后端流播实现处理一个字节区间，合法 Range 返回 `206 Partial Content`，不可满足的区间返回 `416`。普通下载接口返回完整文件。浏览器 `<audio>` 不能自行附加 Bearer Header，因此当前前端会先通过认证请求取得完整音频 Blob，再生成临时 object URL 播放并在切歌时释放；它以等待完整响应和内存占用换取可靠鉴权。若要在前端恢复真正的 Range 边播边下，应改为 HttpOnly Cookie 或短期签名 URL 等媒体元素可以安全使用的认证方式。

WebSocket 遵循 RFC 6455，支持文本、二进制、关闭、Ping/Pong 和分片帧。握手入口为：

```http
GET /ws HTTP/1.1
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: <随机 Base64 值>
Sec-WebSocket-Version: 13
```

## 脚本总览

所有项目脚本都应从仓库根目录执行。带交互菜单的脚本适合人工探索，自动化和文档示例应使用明确子命令。

| 脚本 | 无参数行为 | 推荐显式命令 | 作用 |
|---|---|---|---|
| `setup.sh` | 直接初始化环境 | `bash setup.sh` | 安装开发依赖并执行 `xmake require` |
| `scripts/compile.sh` | 交互菜单 | `bash scripts/compile.sh build` | 构建 C++ 与前端 |
| `scripts/format.sh` | 交互菜单 | `bash scripts/format.sh all` | clang-format 格式化 |
| `scripts/lint.sh` | 直接全量检查 | `bash scripts/lint.sh --all` | clang-tidy、cppcheck、前端 Oxlint |
| `scripts/test.sh` | 直接运行全部测试 | `bash scripts/test.sh` | Google Test 与前端 Vitest |
| `scripts/codeql.sh` | 交互菜单 | `bash scripts/codeql.sh run` | 远程 CodeQL 分析 |
| `scripts/pipeline.sh` | 交互菜单 | `bash scripts/pipeline.sh all` | 执行完整质量流水线 |
| `scripts/benchmark.sh` | 交互菜单 | `bash scripts/benchmark.sh check` | 微基准、QPS、RPS 与报告对比 |
| `scripts/docker.sh` | 交互菜单 | `bash scripts/docker.sh deploy` | 构建、部署和运维 Compose 服务 |
| `scripts/lib/common.sh` | 不应直接运行 | 由其他脚本加载 | 公共菜单、依赖和前端安装函数 |

除无选项的 `scripts/test.sh` 外，用户入口脚本都可通过 `bash <脚本> --help` 查看参数说明；测试脚本的可选位置参数是 xmake 测试目标名。

### 编译脚本

```bash
# 日常增量编译
bash scripts/compile.sh build

# 清除 xmake 配置缓存后编译
bash scripts/compile.sh --clean
```

两个命令都会构建后端和 `frontend/dist/`。前端依赖不存在或锁文件发生变化时，公共脚本会使用 `npm ci` 重新安装。

基准和 Docker 构建会切换 xmake 的构建模式；从 Release 返回日常 Debug 环境时，使用 `bash scripts/compile.sh --clean` 重新配置。

### 格式化脚本

```bash
# 全部 C/C++ 文件
bash scripts/format.sh all

# 指定文件或目录
bash scripts/format.sh core/src/main.cpp net/http tests
```

脚本依次探测 `clang-format-18`、`clang-format-17`、`clang-format-16`、`clang-format`，处理 `.cpp`、`.hpp`、`.h`、`.cc`、`.cxx`，排除 `build/`、`.xmake/` 和编译数据库。工具缺失会失败，不会把跳过当成成功。

### Lint 脚本

```bash
# 全量 C/C++ 检查和前端 Lint
bash scripts/lint.sh --all

# 仅检查相对 HEAD 的 C/C++ 变更，前端仍执行完整 Lint
bash scripts/lint.sh --changed

# 指定范围和并发数
bash scripts/lint.sh -j 8 core net/http tests
```

脚本对 `.cpp`、`.hpp`、`.h`、`.cc`、`.cxx` 执行 clang-tidy 和 cppcheck。缺少或过期的 `compile_commands.json` 会按 xmake 配置自动生成。门禁要求 clang-tidy 的 error/warning/style 和 cppcheck 的 error/warning/style/performance 均为零。

### 测试脚本

```bash
# 全部后端与前端测试
bash scripts/test.sh

# 一个后端测试目标；前端 Vitest 仍会完整运行
bash scripts/test.sh test_tcp_server
```

顶层 `xmake.lua` 从 `tests/*.cpp` 动态创建测试目标，不维护手写目标清单。目标数和用例数会随源码变化，应以当前工作树执行 `bash scripts/test.sh` 的完整输出为准。

前端单元测试由 `scripts/test.sh` 统一执行。真实浏览器验收需要已经部署的 nginx、后端和 MySQL，因此使用独立命令，不会隐式加入普通测试脚本：

```bash
cd frontend
npm run test:e2e

# 可覆盖部署地址和系统 Chromium；产物目录自动带时间戳
PLAYWRIGHT_BASE_URL=http://127.0.0.1:18080 \
PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH=/snap/bin/chromium \
  npm run test:e2e
```

Playwright 固化四个视口：`1440x900`、`1280x800`、`768x1024`、`390x844`。用例覆盖注册、退出、登录、会话恢复、桌面侧栏、移动抽屉、SPA 深链、无效类型前端拦截、上传期间安全追加两个有效 WAV、水平溢出和浏览器控制台错误。报告与截图写入带 `_YYYYMMDD_HHMMSS` 的目录，不创建无时间戳的“最新”别名。

### CodeQL 脚本

```bash
CODEQL_SERVER_URL=http://192.168.1.10:8080 \
  bash scripts/codeql.sh run
```

规定的探测顺序是已设置的 `CODEQL_SERVER_URL`、`http://localhost:8080`、交互输入 IP。当前脚本在已配置地址失效时会跳过 localhost，这是已确认、待修复的 P1 问题，详见 [Step 17 运行时回归 Bug 修复计划](plan/bugfix-step17-runtime-regressions.md)。脚本负责生成和预处理编译数据库、打包源码、提交任务、轮询 SARIF，并以 `0 critical + 0 high` 为通过标准。`analyze` 是 `run` 的兼容别名。

### 完整流水线

```bash
bash scripts/pipeline.sh all
```

`all` 严格按以下顺序执行，任一步失败即停止：

```text
格式化 -> Lint -> 编译 -> CodeQL -> 测试
```

也可单独执行 `format`、`lint`、`compile`、`codeql`、`test` 子命令。流水线包含远程 CodeQL，因此运行前应确保服务可达。

日常开发可先使用以下局部预检缩短反馈时间，但它不替代正式流水线：

```bash
bash scripts/format.sh <修改的文件或目录>
bash scripts/lint.sh --changed
```

正式验收必须执行 `bash scripts/pipeline.sh all`。脚本报告问题时修复业务代码，然后从流水线起点重新执行，确保格式化、全量 Lint、编译、CodeQL 和测试验证的是同一份最终代码。

### 性能脚本

顶层 `xmake.lua` 动态发现 `benchmark/bench_*.cpp` 和 `benchmark/qps_*.cpp`，`scripts/benchmark.sh` 会校验源码与构建目标是否一致。文档不维护易失真的固定目标数量，实时结果以 `bash scripts/benchmark.sh check` 输出为准。

```bash
# 本机依赖和目标发现检查
bash scripts/benchmark.sh check

# 构建性能目标
bash scripts/benchmark.sh build
bash scripts/benchmark.sh build --debug

# 微基准
bash scripts/benchmark.sh micro

# 模块 QPS
bash scripts/benchmark.sh qps smoke
bash scripts/benchmark.sh qps full

# 已部署服务的端到端 RPS
bash scripts/benchmark.sh rps smoke
bash scripts/benchmark.sh rps full
bash scripts/benchmark.sh rps overload

# 对比最近两个同类时间戳报告
bash scripts/benchmark.sh diff micro
bash scripts/benchmark.sh diff qps
bash scripts/benchmark.sh diff rps

# 生成 1KB 到 100MB 的通用测试数据
bash scripts/benchmark.sh gen-data
```

`rps` 不启动服务，执行前应先部署，并通过 `RPS_BASE_URL` 指向同源入口；`load` 是 `rps` 的兼容别名。报告写入 `benchmark/reports/`，文件名均包含 `_YYYYMMDD_HHMMSS` 时间戳，不创建“最新”别名。完整矩阵、环境变量与报告格式见 `benchmark/README.md`。

## Docker 部署

前置条件为 Docker Engine、Docker Compose 插件、OpenSSL、curl、xmake、npm，以及本地已有的 `nginx:latest` 镜像。

```bash
# 构建并启动 MySQL、后端和 nginx，等待健康检查
bash scripts/docker.sh deploy

# 状态与公共入口验证
bash scripts/docker.sh status
bash scripts/docker.sh health

# 输出全部历史日志
bash scripts/docker.sh logs

# 仅输出最近 10 分钟的日志
bash scripts/docker.sh logs --since 10m

# 停止容器，保留 MySQL 和文件数据卷
bash scripts/docker.sh stop
```

`logs --since` 接受 Docker 支持的时长（如 `10m`）或时间戳。脚本不对日志做行级过滤，时间范围由 Docker 解析；不带 `--since` 时仍输出全部历史日志。

首次部署会创建权限为 `0600` 的 `.env`，生成认证密钥、MySQL root 密码和独立应用密码。MySQL 空数据卷通过 `db/schema.sql` 初始化。默认公共入口是：

```text
http://127.0.0.1:18080
```

可在 `.env` 中设置 `HPS_HTTP_PORT` 修改端口。宿主机 `8080` 专用于 CodeQL，部署脚本会拒绝把应用绑定到该端口。`stop` 不删除数据；若确实需要删除数据卷，应单独确认影响后再执行相应 Docker 操作。

`docker.sh build` 只构建本机 Release 后端和前端，`docker.sh image` 还会构建后端镜像但不启动服务。`all`、`up`、`run` 是 `deploy` 的兼容别名，`down` 是 `stop` 的兼容别名。

部署后的完整前端验收应检查根页面、静态资源、`/files` 等 SPA 深链和四个响应式视口。2026-07-22 的修复前版本曾通过 `docker.sh health` 开头公共入口健康检查和真实用户流程 Playwright 验收，有效报告为 `frontend/playwright-report/e2e_20260722_214738/index.html`；该历史结果未覆盖已认证流播，不能代表当前 Step 17 已完成。

## 质量标准

| 检查项 | 通过标准 | 命令 |
|---|---|---|
| 格式化 | clang-format 后无待格式化差异 | `bash scripts/format.sh all` |
| clang-tidy | 0 error、0 warning、0 style | `bash scripts/lint.sh --all` |
| cppcheck | 0 error、0 warning、0 style、0 performance | `bash scripts/lint.sh --all` |
| 编译 | 0 error、0 warning | `bash scripts/compile.sh build` |
| CodeQL | 0 critical、0 high | `bash scripts/codeql.sh run` |
| 测试 | 要求后端与前端全部通过 | `bash scripts/test.sh` |
| 浏览器验收 | 要求四个视口核心流程全部通过 | `cd frontend && npm run test:e2e` |

### 2026-07-22 验证快照

- `bash scripts/lint.sh --changed`：Lint 结果为 `0/0`。
- `bash scripts/compile.sh build`：后端 Release 构建和前端构建均通过。
- `bash scripts/codeql.sh run`：任务 `fa999293-4980-4356-83b3-a2307e87ff18` 完成，`critical=0`、`high=0`。
- `bash scripts/test.sh`：后端 Google Test `41/41` 通过，前端 Vitest `18` 个测试文件、`71` 个用例全部通过。
- Docker 公共入口 `http://127.0.0.1:18080` 健康检查通过。
- 真实部署 Playwright 的 `desktop`、`desktop-compact`、`tablet`、`mobile` 四个项目 `4/4` 通过；最新有效报告为 `frontend/playwright-report/e2e_20260722_214738/index.html`。
- `bash scripts/docker.sh logs --since 5m` 记录到已认证上传返回 HTTP `201`，匿名 stream 请求返回预期的 HTTP `401`。
- 上述 `4/4` 是已执行的历史事实，但用例只覆盖匿名 stream `401`，没有覆盖已认证 stream `200/206`，且只在流程开头检查健康状态。后续复测发现授权流播可触发后端重启并由 nginx 返回 `502`；Step 17 因此被 P0 阻塞，根因和修复验收见 [Step 17 运行时回归 Bug 修复计划](plan/bugfix-step17-runtime-regressions.md)。

这些数字只记录本次工作树曾实际执行的结果，不是永久固定的测试数量或验收门槛；后续仍以测试动态发现结果和脚本原始输出为准。P0 修复前不得把旧报告解释为 Step 17 已完成；修复后必须重新执行全量质量门禁、部署健康检查、授权流播和浏览器验收。

## 许可证

本项目使用 MIT 许可证，见 `LICENSE`。
