# 开发计划

> 基于 `goal.md` 的 Step 进度体系，分阶段实现音乐软件 HTTP 服务器。
> 各步骤详情见独立文件 `plan/step-*.md`。
> **开发阶段完成 → 基准测试 Bug 修复 → 文件上传功能改进 → 并发上传 Bug 修复 → 前端界面 + 后端音乐库重构**

---

## Step 进度总览

| Step | 模块 | 状态 | 详情 |
|------|------|------|------|
| Step 0–2 | 基础设施 + 路由 + HTTP | ✅ 已完成 | [step-0-2-infrastructure.md](step-0-2-infrastructure.md) |
| — | 内存池三级缓存优化 | ✅ 已完成 | [memory-pool-tiered-optimization.md](memory-pool-tiered-optimization.md) |
| Step 2.5 | 架构重构 | ✅ 已完成 | [step-2.5-arch-refactor.md](step-2.5-arch-refactor.md) |
| Step 3 | 接口层落地 | ✅ 已完成 | [step-3-interface-layer.md](step-3-interface-layer.md) |
| Step 4 | file-system | ✅ 已完成 | [step-4-file-system.md](step-4-file-system.md) |
| Step 5 | Range 流式传输 | ✅ 已完成 | [step-5-range-streaming.md](step-5-range-streaming.md) |
| Step 6 | database 数据库连接池 | ✅ 已完成 | [step-6-database.md](step-6-database.md) |
| Step 7 | file-transfer | ✅ 已完成 | [step-7-file-transfer.md](step-7-file-transfer.md) |
| Step 8 | HTTPS/TLS | ✅ 已完成 | [step-8-https-tls.md](step-8-https-tls.md) |
| Step 9 | WebSocket | ✅ 已完成 | [step-9-websocket.md](step-9-websocket.md) |
| Step 10 | LockedThreadPool | ✅ 已完成 | [step-10-locked-thread-pool.md](step-10-locked-thread-pool.md) |
| Step 11 | main.cpp 整合 | ✅ 已完成 | [step-11-main-integration.md](step-11-main-integration.md) |
| Step 12 | 端到端验证 | ✅ 已完成 | [end-to-end-verification.md](end-to-end-verification.md) |
| — | 基准测试 Bug 修复 | ✅ 已完成 | [bugfix-memory-pool-thread-pool.md](bugfix-memory-pool-thread-pool.md) |
| Step 13 | Docker 化部署 | ✅ 已完成 | 参见 goal.md |
| Step 14 | 文件上传功能改进 | ✅ 已完成 | [step-14-file-upload-improvement.md](step-14-file-upload-improvement.md) |
| — | 并发上传 Bug 修复 | ✅ 已完成 | [bugfix-concurrent-uploads.md](bugfix-concurrent-uploads.md) |
| Step 15 | 前端 Web 界面 (Crystal Music) | ✅ 已完成 | [step-15-frontend.md](step-15-frontend.md) |
| Step 16 | 后端数据库重构 + 音乐库/歌单 API | ✅ 已完成 | [step-16-backend-music.md](step-16-backend-music.md) |
| — | **全方位测试 + 微基准覆盖** | ✅ **已完成** | 详见 goal.md |
| — | Fix: Prepared Statement 查询失败 | ✅ 已完成 | [fix-prepared-statement.md](fix-prepared-statement.md) |
| — | Fix: TcpClient 端口竞争 (SO_REUSEADDR) | ✅ 已完成 | 详见 goal.md |
| — | Fix: main.cpp 拆分为可测函数 | ✅ 已完成 | 详见 goal.md |

---

## 质量门禁（每 Step 严格执行）

- [x] clang-tidy: 0 error + 0 warning + 0 style
- [x] cppcheck --enable=all: 0 error + 0 warning + 0 style + 0 performance
- [x] 编译: 0 error + 0 warning
- [x] xmake test: 100% 通过 (37 二进制, 39+ 测试套)
- [x] CodeQL: 0 critical + 0 high severity
