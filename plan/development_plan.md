# 开发计划

> 本文是项目阶段总览。各阶段的设计与历史实施记录见 `plan/step-*.md`；当前运行、验证和部署入口以本文、根目录 `README.md` 与 `scripts/` 实际文件为准。
> “实现已落地”表示代码已经进入当前工作树，不等同于正式全仓质量门禁或真实浏览器验收已经通过。

## Step 进度总览

| Step | 模块 | 状态 | 详情 |
|------|------|------|------|
| Step 0-2 | 基础设施、路由与 HTTP | 已完成 | [step-0-2-infrastructure.md](step-0-2-infrastructure.md) |
| - | 内存池三级缓存优化 | 已完成 | [memory-pool-tiered-optimization.md](memory-pool-tiered-optimization.md) |
| Step 2.5 | 架构重构 | 已完成 | [step-2.5-arch-refactor.md](step-2.5-arch-refactor.md) |
| Step 3 | 接口层落地 | 已完成 | [step-3-interface-layer.md](step-3-interface-layer.md) |
| Step 4 | file-system | 已完成 | [step-4-file-system.md](step-4-file-system.md) |
| Step 5 | Range 流式传输 | 已完成 | [step-5-range-streaming.md](step-5-range-streaming.md) |
| Step 6 | database 数据库连接池 | 已完成 | [step-6-database.md](step-6-database.md) |
| Step 7 | file-transfer | 已完成 | [step-7-file-transfer.md](step-7-file-transfer.md) |
| Step 8 | HTTPS/TLS | 已完成 | [step-8-https-tls.md](step-8-https-tls.md) |
| Step 9 | WebSocket | 已完成 | [step-9-websocket.md](step-9-websocket.md) |
| Step 10 | LockedThreadPool | 已完成 | [step-10-locked-thread-pool.md](step-10-locked-thread-pool.md) |
| Step 11 | `main.cpp` 整合 | 已完成 | [step-11-main-integration.md](step-11-main-integration.md) |
| Step 12 | 端到端验证 | 历史阶段已完成 | [end-to-end-verification.md](end-to-end-verification.md)，其中旧 `verification/` 工具已经删除 |
| - | 基准测试问题修复 | 已完成 | [bugfix-memory-pool-thread-pool.md](bugfix-memory-pool-thread-pool.md) |
| Step 13 | Docker 化部署 | 已完成，持续维护 | [docker-deployment.md](docker-deployment.md) |
| Step 14 | 文件上传功能改进 | 已完成 | [step-14-file-upload-improvement.md](step-14-file-upload-improvement.md) |
| - | 并发上传问题修复 | 已完成 | [bugfix-concurrent-uploads.md](bugfix-concurrent-uploads.md) |
| Step 15 | 前端 Web 界面第一版 | 历史基线 | [step-15-frontend.md](step-15-frontend.md)，后续以 Step 17 为准 |
| Step 16 | 后端数据库重构与音乐库/歌单 API | 已完成 | [step-16-backend-music.md](step-16-backend-music.md) |
| - | Prepared Statement 查询修复 | 已完成 | [fix-prepared-statement.md](fix-prepared-statement.md) |
| Step 17 | 前端体验、上传契约与浏览器验收补强 | P0 运行时回归阻塞，修复待执行 | [step-17-frontend-optimization.md](step-17-frontend-optimization.md)、[bugfix-step17-runtime-regressions.md](bugfix-step17-runtime-regressions.md) |

## Step 17 当前范围

- 上传端到端统一为原始文件字节，并通过 `Content-Disposition: filename*` 传递 UTF-8 文件名；前后端共同校验扩展名、空文件和角色大小上限，后端继续作为最终安全边界。
- 前端上传队列支持稳定标识、受控并发、上传期间继续追加、取消、重试、移除和准确汇总，并保留后端 JSON、纯文本或 HTML 错误详情。
- 认证恢复、角色归一化、受保护的下载与播放、播放器资源回收、响应式导航、页面状态、表单约束和无障碍交互已补强。
- 已加入 Vitest 回归用例和部署环境 Playwright 配置；真实浏览器验收依赖 nginx、后端和 MySQL，不并入普通单元测试脚本。
- 2026-07-22 分项质量门禁和四视口 Playwright `4/4` 是已执行的历史事实，但 E2E 只覆盖匿名 stream `401` 且只在开头检查健康。后续复测确认已认证 stream 可触发后端重启和 nginx `502`，P0 修复与重新验收前不得宣告 Step 17 完成。

## 当前稳定入口

| 场景 | 非交互命令 | 说明 |
|------|------------|------|
| 格式化 | `bash scripts/format.sh all` | 格式化仓库受管源码 |
| Lint | `bash scripts/lint.sh --all` | clang-tidy、cppcheck 与前端 Oxlint |
| 构建 | `bash scripts/compile.sh build` | 构建 C++ 后端与前端生产包 |
| 测试 | `bash scripts/test.sh` | 运行全部 Google Test 与前端 Vitest |
| CodeQL | `bash scripts/codeql.sh run` | 探测并使用 CodeQL 服务 |
| 正式流水线 | `bash scripts/pipeline.sh all` | 格式化、Lint、构建、CodeQL、测试，失败即停止 |
| Docker | `bash scripts/docker.sh deploy` | 构建并启动 MySQL、后端和 nginx；另有 `status`、`health`、`logs`、`stop` |
| 基准测试 | `bash scripts/benchmark.sh <子命令>` | 运行微基准、模块 QPS 或 HTTP RPS 场景 |

历史 `verification/` 目录和 `scripts/run_tp_debug.sh` 已删除，不再是当前入口。部署后的用户流程由 Playwright 验证；GDB 调试指定目标使用 `xmake run -d <target>`。

## 端口约定

- 应用公共入口：`http://127.0.0.1:18080`，由 nginx 暴露；可通过 `.env` 的 `HPS_HTTP_PORT` 修改。
- CodeQL 服务：`http://localhost:8080`，或使用 `CODEQL_SERVER_URL` 指定的地址。
- `scripts/docker.sh` 会拒绝把应用公共入口绑定到为 CodeQL 保留的 `8080`。

## 最终质量门禁

仓库长期稳定的完整流水线入口为：

```bash
bash scripts/pipeline.sh all
```

该命令固定执行：

```text
格式化 -> Lint --all -> 后端与前端构建 -> CodeQL -> 后端与前端测试
```

任一步修改代码后，都必须从完整流水线起点重新执行。测试目标由 `xmake.lua` 和测试目录动态发现，文档不维护容易失真的固定目标数或用例数。

Step 17 在 2026-07-22 的修复前验收实际依次执行了四个分项门禁，没有另行执行 `bash scripts/pipeline.sh all`。以下数量仅为该次执行快照，不取代上述动态发现原则，也不代表当前 P0 已通过：

| 命令 | 2026-07-22 执行结果 |
|------|----------------------|
| `bash scripts/lint.sh --changed` | 通过；clang-tidy 与 cppcheck 门禁结果 0/0，前端 Lint 通过 |
| `bash scripts/compile.sh build` | 后端 Release 构建与前端生产构建均通过 |
| `bash scripts/codeql.sh run` | 任务 `fa999293-4980-4356-83b3-a2307e87ff18` 通过，`critical=0`、`high=0` |
| `bash scripts/test.sh` | 后端 Google Test 41/41，前端 Vitest 18 个测试文件、71 个用例全部通过 |

真实浏览器验收在部署健康后单独执行：

```bash
bash scripts/docker.sh deploy
cd frontend
PLAYWRIGHT_BASE_URL=http://127.0.0.1:18080 npm run test:e2e
cd ..
bash scripts/docker.sh logs
```

最终标准为：clang-tidy 与 cppcheck 无门禁级问题，编译无错误和警告，CodeQL 无 critical/high，后端与前端自动化测试全部通过，配置的桌面与移动视口 E2E 全部通过且浏览器控制台没有未处理错误。

2026-07-22 的部署验收中，Docker 公共入口 `http://127.0.0.1:18080` 开头健康检查通过；Playwright 的 `desktop`、`desktop-compact`、`tablet`、`mobile` 四个项目 4/4 通过，0 失败、0 不稳定、0 跳过，有效报告目录为 `e2e_20260722_214738`。`bash scripts/docker.sh logs --since 5m` 同时记录到上传请求 `201` 和匿名流式请求的预期 `401`。这些结果未覆盖已认证 stream `200/206`、用例末尾健康状态或容器 `RestartCount`；后续复测暴露 P0 崩溃，因此 Step 17 当前阻塞，实施与验收以 [运行时回归 Bug 修复计划](bugfix-step17-runtime-regressions.md) 为准。
