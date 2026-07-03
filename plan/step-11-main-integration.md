# Step 11：main.cpp 整合启动

> **状态**：待开始
> **优先级**：P0

## 背景

所有模块开发完成后，实现 `main.cpp` 整合启动流程，包括服务初始化、路由注册、配置管理。

## 功能点

| # | 功能点 | 优先级 | 说明 |
|---|--------|--------|------|
| F1 | **服务器启动流程** | P0 | Logger → 各模块 init → start → stop → shutdown |
| F2 | **路由注册** | P0 | 注册所有业务路由（用户/歌曲/播放列表等）|
| F3 | **配置管理** | P0 | 命令行参数 / 配置文件读取 |

## 启动流程

```
main() {
  Logger::init("music-server");           // 1. 最先初始化
  DbConfig db_cfg = load_config("config.json");
  DatabasePool db;
  db.init(db_cfg);                        // 2. 数据库连接池
  FileSystem fs(root_dir);
  HttpServer server(cfg);
  register_routes(server, db, fs);        // 3. 路由注册
  server.init();
  server.start();                         // 4. 启动服务
  wait_for_signal();                      // 5. 等待停止信号
  server.stop();                          // 6. 停止服务
  db.close();
  Logger::shutdown();                     // N. 最后销毁
}
```

## 文件清单

| 操作 | 路径 |
|------|------|
| 创建 | `core/src/main.cpp` |
| 创建 | `config.json` 配置文件模板 |

## 测试

本步骤不涉及单元测试，通过端到端启动验证。

## 质量门禁

| 检查项 | 要求 |
|--------|------|
| 编译 | ✅ 0 error / 0 warning |
| 启动 | 服务正常启动，路由可访问 |
