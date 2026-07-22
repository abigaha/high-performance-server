# 端到端验证指南

> 本文给出当前仓库可执行的验证入口。早期 `verification/` 目录已经删除；其中的脚本和用例编号仅属于历史，不能再作为当前验收命令或通过证据。

## 验证分层

| 层级 | 入口 | 覆盖范围 |
|------|------|----------|
| 后端与前端单元测试 | `bash scripts/test.sh` | 全部 xmake/Google Test 目标与前端 Vitest |
| 正式质量流水线 | `bash scripts/pipeline.sh all` | 格式化、全量 Lint、构建、CodeQL、全部测试 |
| 部署健康检查 | `bash scripts/docker.sh deploy`、`health`、`status` | MySQL、后端、nginx 和公共健康链路 |
| 真实浏览器 E2E | `frontend` 下的 `npm run test:e2e` | 注册登录、会话、导航、上传和响应式界面 |
| 运行日志 | `bash scripts/docker.sh logs` | 三个 Compose 服务的完整日志 |

`scripts/test.sh` 不运行 Playwright。浏览器 E2E 会依赖真实 nginx、后端和 MySQL，还会写入测试用户及上传记录，因此必须显式部署后执行。

## 推荐执行顺序

### 1. 运行后端与前端测试

```bash
bash scripts/test.sh
```

需要正式发布前的完整门禁时，使用项目统一流水线：

```bash
bash scripts/pipeline.sh all
```

流水线固定执行 `format -> lint --all -> compile -> CodeQL -> test`，任一步失败即停止。不要用本文中的单独命令组合替代正式流水线的质量结论。

### 2. 部署真实服务

```bash
bash scripts/docker.sh deploy
bash scripts/docker.sh status
bash scripts/docker.sh health
```

默认应用入口是 `http://127.0.0.1:18080`。`http://localhost:8080` 属于 CodeQL，不是应用入口。部署脚本会构建 Release 后端和前端、启动 MySQL/后端/nginx、等待健康状态，并从 nginx 公共入口检查 `/api/health`。

### 3. 运行 Playwright

```bash
cd frontend
PLAYWRIGHT_BASE_URL=http://127.0.0.1:18080 npm run test:e2e
```

若需要显式使用系统 Chromium，可增加：

```bash
PLAYWRIGHT_BASE_URL=http://127.0.0.1:18080 \
PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH=/snap/bin/chromium \
npm run test:e2e
```

Playwright 当前固定四个视口：

| 项目 | 视口 |
|------|------|
| `desktop` | `1440x900` |
| `desktop-compact` | `1280x800` |
| `tablet` | `768x1024` |
| `mobile` | `390x844` |

当前部署用例覆盖：

- nginx 根页面、`/api/health` 和 SPA 深链；
- 注册、退出、登录以及刷新后的会话恢复；
- 桌面侧栏和移动抽屉导航；
- 注册页、登录页、文件页和上传页的水平溢出检查；
- `.txt` 文件在前端被拦截且不会产生上传请求；
- 第一个 WAV 正在上传时追加第二个 WAV，两个请求都成功进入调度；
- 上传响应中的文件名和大小；
- 浏览器控制台与页面未处理错误。

用例会为每个视口创建唯一测试用户，并上传两个带随机后缀的 WAV。`scripts/docker.sh stop` 会保留 `mysql_data` 和 `app_data`，因此这些记录不会随停止容器自动删除。

Playwright 的运行目录和 HTML 报告目录包含 `_YYYYMMDD_HHMMSS` 时间戳，不创建无时间戳的“最新”别名。失败时还会保留 trace、截图和视频；用例本身也保存关键页面截图。

### 4. 检查服务日志

```bash
bash scripts/docker.sh logs
```

日志必须完整检查，重点确认注册、登录、上传和下载请求没有后端异常、数据库错误、容器重启或 nginx 代理失败。不要用行级截断后的日志代替完整输出。

### 5. 结束部署

```bash
bash scripts/docker.sh stop
```

该命令移除容器但保留数据卷。当前脚本没有清空数据卷的日常子命令，避免误删测试或用户数据。

## 验收标准

- `scripts/test.sh` 中后端与前端测试全部通过；
- 正式发布时 `scripts/pipeline.sh all` 的五个阶段全部通过；
- Docker 三个服务均为 `running healthy`，公共健康接口返回 `status: ok`；
- Playwright 四个视口全部通过，无非预期控制台或页面错误；
- 无效文件没有发出上传请求，上传期间追加的两个有效 WAV 均返回 `201`；
- 完整 Docker 日志没有与验收流程相关的服务端异常。

本文不预填任何一次执行结果。每轮验收必须记录实际命令、时间、通过数量、失败原因和产物目录，不能沿用旧文档中的固定测试数量。

## 历史说明

已删除的 `verification/verify.sh` 曾覆盖 Keep-Alive、文件全生命周期、并发请求和方法边界，`verification/ws_test.py` 曾用于 WebSocket 检查。这些名称仅用于理解历史提交；当前仓库中没有对应可执行文件。相关行为应由 Google Test、Vitest、Playwright 或现行脚本覆盖，禁止在使用说明中要求运行 `verification/*`。
