# Step 2.5：架构重构

> **状态**：✅ 已完成（commit `53a4518`）
> **起止**：路由模块完成后

## 背景

代码中存在命名不规范、内存池接口低效、函数对象性能不佳等问题，进行架构层面的重构以提升可维护性和性能。

## 功能点

| # | 功能点 | 说明 |
|---|--------|------|
| F1 | **命名规范统一** | 类名去 C 前缀、snake_case 方法、k+PascalCase 常量等 |
| F2 | **内存池 CRTP 静态多态** | 去 virtual 析构，改 deleter 静态分发，高频路径零虚函数开销 |
| F3 | **MoveOnlyFunction SBO 优化** | 小对象内联存储（SBO，Small Buffer Optimization），减少堆分配 |

## 文件清单

| 路径 | 说明 |
|------|------|
| `memory-pool/include/memory_pool_base.h` | CRTP 基类（重构） |
| `memory-pool/include/tiered_memory_pool.h` | 分层内存池（重构） |
| `memory-pool/include/memory_pool_factory.h` | 工厂 + 静态 deleter（新增） |
| `memory-pool/src/tiered_memory_pool.cpp` | 实现（重构） |
| `net/thread-pool/include/move_only_function.h` | SBO 优化（重构） |
| 全项目源文件 | 命名规范统一（重命名） |

## 质量门禁

| 检查项 | 结果 |
|--------|------|
| clang-tidy | ✅ 0 / 0 / 0 |
| cppcheck | ✅ 0 / 0 / 0 / 0 |
| 编译 | ✅ 0 error / 0 warning |
| 测试 | ✅ 100% passing |
