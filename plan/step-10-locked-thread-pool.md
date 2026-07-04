# Step 10：LockedThreadPool 有锁通用线程池

> **状态**：✅ 已完成
> **优先级**：P1

## 背景

现有 LockFreeThreadPool 基于 `LockFreeQueue` 无锁队列，适用于高频低竞争场景。在 IO 密集型或任务队列较长场景下，需要一版基于 `std::queue` + `std::mutex` 的有锁线程池作为通用备选。

## 实现方案

### 类关系

```
ThreadPoolBase<Derived>  (CRTP 基类，静态多态)
  ├── LockFreeThreadPool  (已有：无锁队列)
  └── LockedThreadPool    (当前：有锁队列)
```

### 接口签名

```cpp
class LockedThreadPool : public ThreadPoolBase<LockedThreadPool> {
public:
  explicit LockedThreadPool(std::size_t num_threads);
  ~LockedThreadPool();

  // CRTP enqueue（模板方法，hpp 内联）
  template <typename F, typename... Args>
    requires std::invocable<F, Args...>
  void enqueue_impl(F&& f, Args&&... args);

  void wait_impl();                                   // 无限等待全部完成
  bool wait_for(std::chrono::milliseconds timeout);   // 超时等待
  void stop_impl();                                   // 优雅关闭

private:
  void worker(std::stop_token stop_token);            // 工作线程函数

  std::vector<std::jthread> workers_;                 // jthread + stop_token
  std::queue<MoveOnlyFunction> tasks_;                // 有锁任务队列
  std::mutex queue_mutex_;                            // 队列 + cv 共享锁
  std::condition_variable cv_;                        // 条件变量通知
  std::atomic<int> pending_{0};                       // 待处理任务计数
  bool stop_{false};                                  // 停止标志
};
```

### 关键设计决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| 线程类型 | `std::jthread` + `stop_token` | 与 LockFreeThreadPool 一致，自动 join |
| 等待机制 | `pending_` 原子计数 | 不能用 `queue.size()`（worker 取走后但在执行中队列空）|
| stop 清空 | drain 队列 + `pending_` 置 0 | 防止 worker break 后 pending 遗留导致 wait 死等 |
| 超时等待 | `wait_for(ms)` 扩展方法 | 不修改基类 `ThreadPoolBase`；返回 bool 指示超时 |
| 异常安全 | worker 内 try-catch + Logger | catch `std::exception` 和 `...`，调用 `Logger::_error` 记录 |
| 依赖 | `add_deps("logger")` | worker 异常日志依赖 Logger 两阶段单例（未 init 时静默丢弃） |

### 核心实现细节

**`enqueue_impl`**（模板内联于 .h）：
```
fold lambda 完美转发 → lock → stop_ 检查 → emplace → unlock → pending++ → notify_one
```
- 模板方法必须在头文件内联（CRTP + 编译期多态要求）
- `stop_` 检查在持锁下进行，保证线程安全

**`worker`** 主循环：
```
while (!stop_requested)
  lock → cv.wait(stop_ || !tasks_.empty() || stop_requested)
         → stop/stop_requested → break
         → empty → continue（spurious wakeup）
         → pop task → unlock
  try { task() }
  catch (std::exception& e)  → Logger::_error
  catch (...)                → Logger::_error
  pending-- → cv_.notify_all
```

**`stop_impl`**：
```
lock → stop_=true → swap 清空队列 → pending_=0
notify_all → request_stop → join all
```

### 与 LockFreeThreadPool 对比

| 维度 | LockFreeThreadPool | LockedThreadPool |
|------|-------------------|------------------|
| 队列 | `LockFreeQueue`（无锁） | `std::queue` + `mutex` |
| 线程 | `std::jthread` | `std::jthread`（一致）|
| CV | `cv_mutex_` 专用互斥量 | `queue_mutex_` 复用于队列 |
| pending | `std::atomic<int>` | `std::atomic<int>`（一致）|
| 停后入队 | push 失败（queue stopped） | lock → stop_ 检查 → return |
| wait_for | 无 | `wait_for(ms)` 扩展 |
| 异常保护 | 无（terminate） | try-catch + Logger::_error |
| 适用场景 | 高频低竞争、延迟敏感 | IO密集、长队列、通用场景 |

## 文件清单

| 操作 | 路径 | 行数 |
|------|------|------|
| CREATE | `net/thread-pool/include/locked_thread_pool.h` | 62 |
| CREATE | `net/thread-pool/src/locked_thread_pool.cpp` | 76 |
| CREATE | `tests/test_locked_thread_pool.cpp` | 103 |
| MODIFY | `net/thread-pool/xmake.lua` | +`add_deps("logger")` |

## 测试用例

| # | 测试 | 说明 |
|---|------|------|
| T1 | BasicTaskExecution | 单任务提交，验证结果 42 |
| T2 | ConcurrentTasks | 4 线程池 + 1000 次 fetch_add，全部完成 |
| T3 | WaitForAllTasks | 10 个 10ms 任务，wait_impl 等待后检查 |
| T4 | WaitForTimeoutReturnsFalse | 500ms 长任务 + 50ms 超时，应返回 false |
| T5 | WaitForReturnsTrue | 10ms 短任务 + 5s timeout，应返回 true |
| T6 | StopGracefully | 50 个 5ms 任务，stop 后线程退出无死锁 |
| T7 | EnqueueAfterStop | stop 后 enqueue，任务不被执行 |
| T8 | RaiiDestructor | 不调 stop 直接析构，RAII 安全 |

## 质量门禁结果

| 检查项 | 结果 |
|--------|------|
| clang-tidy | 0 error + 0 warning + 0 style |
| cppcheck --enable=all | 0 error + 0 warning + 0 style + 0 performance |
| 编译 | 0 error + 0 warning |
| CodeQL | 0 critical + 0 high |
| xmake test | 20/20 (100%) 通过，含 test_locked_thread_pool (8 用例) |
