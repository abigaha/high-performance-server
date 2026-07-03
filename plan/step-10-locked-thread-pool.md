# Step 10：LockedThreadPool 有锁通用线程池

> **状态**：待开始
> **优先级**：P1

## 背景

现有 LockFreeThreadPool 基于无锁队列，适用于高频低竞争场景。但在某些场景（如 IO 密集型任务、任务队列较长）可能需要有锁线程池，基于 `std::queue` + `std::mutex` 实现。

## 功能点

| # | 功能点 | 优先级 | 说明 |
|---|--------|--------|------|
| F1 | **LockedThreadPool 实现** | P0 | 继承 ThreadPoolBase<LockedThreadPool> |
| F2 | **条件变量通知** | P0 | 任务入队通知工作线程 |
| F3 | **优雅关闭** | P0 | stop() 等待进行中任务完成 |

## 接口设计

```cpp
class LockedThreadPool : public ThreadPoolBase<LockedThreadPool> {
public:
  LockedThreadPool(std::size_t num_threads);
  ~LockedThreadPool();

  template <typename F, typename... Args>
    requires std::invocable<F, Args...>
  void enqueue_impl(F&& f, Args&&... args);

  void wait_impl();
  void stop_impl();

private:
  std::vector<std::thread> workers_;
  std::queue<MoveOnlyFunction> tasks_;
  std::mutex queue_mutex_;
  std::condition_variable cv_;
  bool stop_{false};
};
```

## 文件清单

| 操作 | 路径 |
|------|------|
| 创建 | `net/thread-pool/include/locked_thread_pool.h` |
| 创建 | `net/thread-pool/src/locked_thread_pool.cpp` |
| 创建 | `tests/test_locked_thread_pool.cpp` |

## 测试用例（预估）

| # | 说明 |
|---|------|
| T1 | 提交任务并执行 |
| T2 | 多线程任务执行 |
| T3 | wait_for_all_tasks 等待 |
| T4 | stop 优雅关闭 |
