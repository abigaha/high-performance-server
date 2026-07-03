# 内存池三级缓存优化 — Plan

## 目标
将服务器直接调用 `MemoryPool` 的行为改为通过抽象接口 `IMemoryPool`，并实现三级缓存（L1 线程缓存 → L2 中心缓存 → L3 页缓存）的新内存池。

## 架构

```
调用方 (coroitem.hpp)
    ↓ IMemoryPool 接口
TieredMemoryPool
    ├─ L1: thread_local ThreadCache (每个 size class 32 块上限)
    ├─ L2: CentralCache (互斥锁保护, 批量搬运)
    └─ L3: PageCache (::operator new 分配整页)
```

### 合并策略
- L1 某 size class 桶满 → 尝试合并相邻块 → 升入更大 size class
- 合并后仍满 → 批量归还到 L2
- L2 也满 → L2 内合并 + 递归上升

## 文件清单

| 文件 | 说明 |
|------|------|
| `memory-pool/include/i_memory_pool.h` | 抽象接口 |
| `memory-pool/include/size_class.h` | SizeClass 常量与工具函数 |
| `memory-pool/include/tiered_memory_pool.h` | 三级缓存类声明 |
| `memory-pool/src/tiered_memory_pool.cpp` | 三级缓存实现 |
| `memory-pool/include/memory_pool_factory.h` | 工厂函数声明 |
| `memory-pool/src/memory_pool_factory.cpp` | 工厂函数实现 |
| `tests/test_memory_pool.cpp` | 8 个单元测试 |

## 质量门禁
- clang-tidy: 0 error + 0 warning
- cppcheck --enable=all: 0 error + 0 warning + 0 style
- CodeQL: 0 critical + 0 high
- 编译: 0 error + 0 warning
- 测试: 8/8 通过
