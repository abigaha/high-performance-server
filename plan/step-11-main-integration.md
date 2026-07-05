# Step 11：main.cpp 整合启动

> **状态**：✅ 已完成
> **优先级**：P0

## 背景

所有模块开发完成后，实现 `main.cpp` 整合启动流程，包括服务初始化、路由注册、配置管理。

## 功能点

| # | 功能点 | 优先级 | 说明 |
|---|--------|--------|------|
| F1 | **服务器启动流程** | P0 | Logger → 各模块 init → start → stop → shutdown |
| F2 | **路由注册** | P0 | 注册所有业务路由（用户/歌曲/播放列表等）|
| F3 | **配置管理** | P0 | 命令行参数 / 配置文件读取 |

## 实现摘要

### F1: 服务器启动流程

```
main() {
  Logger::init("music-server");              // 1. 最先初始化
  load_config(argc, argv);                    // 2. 加载配置 (config.json + cmd args)
  DatabasePool(MockConnection 工厂);          // 3. Mock 数据库连接池
  FileSystem(fs_root_dir);                   // 4. 文件系统
  HttpServer(tcp_cfg);                       // 5. HTTP 服务器
  register_routes(server, db, fs);           // 6. 路由注册
  server.init();                             // 7. 初始化（绑定端口）
  std::signal(SIGINT/SIGTERM, handler);      // 8. 注册信号处理
  auto future = g_shutdown_promise.get_future();
  server.start();                            // 9. 启动（非阻塞）
  future.wait();                             // 10. 阻塞等待信号
  server.stop();                             // 11. 停止服务
  db.close();                                // 12. 关闭数据库
  Logger::shutdown();                        // N. 最后销毁
}
```

信号处理使用 `std::promise<void>` + `std::future::wait()` + `extern "C"` signal handler，信号仅设原子标志 + fulfill promise，实际关闭在主线程执行。

### F2: 路由注册

| 方法 | 路径 | 行为 |
|------|------|------|
| GET | `/api/health` | 返回 `{"status":"ok","uptime":N}` |
| GET | `/api/users/:id` | `db.get_user(id)` → JSON 200 / 404 |
| POST | `/api/users` | `db.create_user(u)` → 201 / 500 |
| GET | `/api/users/:id/history` | `db.get_download_history(id)` → JSON 数组 |
| GET | `/api/files/:hash` | `db.get_file_meta(hash)` → JSON 200 / 404 |
| POST | `/api/files/upload` | 上传文件 → 201 `{"hash":"...","size":N}` / 200 `{"exists":true}` |
| GET | `/api/files/:hash/download` | 下载文件 → 200 octet-stream / 404 |
| WS | `/ws` | 握手 → WsConnection 帧日志打印 |

handler 实现完整 CRUD 逻辑，底层使用 MockConnection 工厂（无需真实 MySQL）。

### F3: 配置管理

**config.json** (nlohmann/json 解析):
- `server.port`, `server.thread_count`, `server.backlog`, `server.epoll_timeout_ms`
- `database.host`, `database.port`, `database.username`, `database.password` 等
- `ssl.enabled`, `ssl.cert_file`, `ssl.key_file` 等
- `filesystem.root_dir`

**命令行参数覆盖**: `--port`, `--config`, `--threads`, `--db-host`, `--db-port`, `--data-dir`, `--help`

## 文件清单

| 操作 | 路径 |
|------|------|
| 创建 | `core/src/main.cpp` — 完整服务器生命周期 |
| 创建 | `config.json` — 配置文件模板 |
| 创建 | `db/include/mock_connection.h` — MockConnection 提取为公共头文件 |
| 修改 | `xmake.lua` — 添加 `add_requires("nlohmann_json")` |
| 修改 | `core/xmake.lua` — 添加 `http/db/file-system` deps |

## 质量门禁

| 检查项 | 结果 |
|--------|------|
| clang-tidy | 0 error + 0 warning + 0 style |
| cppcheck | 0 error + 0 warning + 0 style + 0 performance |
| 编译 | 0 error / 0 warning |
| CodeQL | 0 critical + 0 high |
| 测试 | 100% 通过（20 二进制, ~167 用例） |
