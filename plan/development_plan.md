# 开发计划

> 基于 `goal.md` 的 Step 进度体系，分阶段实现音乐软件 HTTP 服务器。
> 各步骤详情见独立文件 `plan/step-*.md`。
> 当前进度：Step 8（HTTPS/TLS）已完成，Step 9（WebSocket）为当前关卡。

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
| **Step 9** | **WebSocket** | **← 当前** | [step-9-websocket.md](step-9-websocket.md) |
| Step 10 | LockedThreadPool | 待开始 | [step-10-locked-thread-pool.md](step-10-locked-thread-pool.md) |
| Step 11 | main.cpp 整合 | 待开始 | [step-11-main-integration.md](step-11-main-integration.md) |

---

## 质量门禁（每 Step 严格执行）

- [ ] clang-tidy: 0 error + 0 warning + 0 style
- [ ] cppcheck --enable=all: 0 error + 0 warning + 0 style + 0 performance
- [ ] 编译: 0 error + 0 warning
- [ ] xmake test: 100% 通过
- [ ] CodeQL: 0 critical + 0 high severity
