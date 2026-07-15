# Bug 改进 Plan：MemoryPool 死锁 + LockFreeThreadPool 超时

## Bug 1：`MemoryPool_BatchAllocate` 卡死

### 现象

`BM_MemoryPool_BatchAllocate` 在第 3 次 benchmark 迭代（分配+释放 100 块，重复 3 轮）时永久卡死，进程不退出也不超时。

### 根因分析

卡死触发路径：

```
deallocate_impl(cache.counts_[classIdx] > kMaxBlocksPerClass)
  → try_merge_and_promote(cache, classIdx)        // 递归，最多 10 层 (kSizeClassCount)
    → try_merge_in_list(list, count, blockSize, merged)  // O(n²) 双重循环遍历链表
      → page_index_of(lower, lowerIdx)             // 每对候选节点 → 线性扫描 pages_
        → lock_guard(pagesMutex_)                  // ◆ 每次调用取锁
        → for (i = 0; i < pages_.size(); ++i)      // ◆ 线性扫描，pages_ 持续增长
```

三个因素叠加导致卡死：

| 因素 | 说明 |
|------|------|
| **① 链表操作导致链表损坏/成环** | `try_merge_in_list` 移除相邻节点时，通过 `*prev = a->next` 和 `*innerPrev = b->next` 修改链表指针。当一个节点既出现在外层 `prev` 路径上、又出现在内层 `innerPrev` 路径上时，移除后可能导致回环。尤其小 size class（8B/16B/32B）下，大量连续地址块反复合并/晋升/回退，链表拓扑结构恶化 |
| **② `try_merge_and_promote` 无迭代上限** | 递归无最大深度/次数保护。小 block 反复合并晋升到高 class，再从高 class 返回低 class 分配，形成活锁 |
| **③ `page_index_of` 高频率线性扫描** | 每次 `try_merge_in_list` 内每对候选节点都调 `page_index_of`，而 `page_index_of` 对 `pages_` 做无界线性扫描（`O(n)`）× 每次都取 `pagesMutex_`。`pages_` 随分配不断增长，扫描耗时递增，导致函数实际不返回 |

### 改进方案

#### 1.1 链表遍历添加循环检测（防御性）

**文件：** `memory-pool/src/tiered_memory_pool.cpp:217`

在 `try_merge_in_list` 链表遍历中加入 **快慢指针循环检测**，发现成环立即退出：

```cpp
bool TieredMemoryPool::try_merge_in_list(FreeNode*& list, std::size_t& count, ...) {
  if (list == nullptr || list->next == nullptr) {
    return false;
  }
  // 快慢指针循环检测
  auto* slow = list;
  auto* fast = list;
  while (fast != nullptr && fast->next != nullptr) {
    slow = slow->next;
    fast = fast->next->next;
    if (slow == fast) {
      return false;  // 检测到环，安全退出
    }
  }
  ...  // 原有合并逻辑
}
```

#### 1.2 `try_merge_and_promote` 改为迭代 + 上限

**文件：** `memory-pool/src/tiered_memory_pool.cpp:194`

改递归为迭代，添加 `kMaxMergeIterations` 上限防止活锁：

```cpp
void TieredMemoryPool::try_merge_and_promote(ThreadCache& cache, std::size_t classIdx) {
  constexpr int kMaxMergeIterations = 100;
  int iter = 0;

  std::size_t currentIdx = classIdx;
  while (currentIdx < kSizeClassCount - 1 && iter++ < kMaxMergeIterations) {
    FreeNode* merged = nullptr;
    if (!try_merge_in_list(cache.freeLists_[currentIdx], cache.counts_[currentIdx],
                           size_class_size(currentIdx), merged)) {
      break;
    }
    auto const nextIdx = currentIdx + 1;
    merged->next = cache.freeLists_[nextIdx];
    cache.freeLists_[nextIdx] = merged;
    cache.counts_[nextIdx]++;

    if (cache.counts_[nextIdx] > kMaxBlocksPerClass) {
      currentIdx = nextIdx;
    } else {
      break;
    }
  }
}
```

#### 1.3 `page_index_of` 改用二分查找

**文件：** `memory-pool/src/tiered_memory_pool.cpp:308`

当前线性扫描是热点瓶颈。改为 `pages_` 按 `base` 地址排序后二分查找：

`central_allocate_batch` 插入时保持排序：

```cpp
void TieredMemoryPool::central_allocate_batch(std::size_t classIdx) {
  ...
  {
    std::lock_guard lock(pagesMutex_);
    auto it = std::lower_bound(pages_.begin(), pages_.end(), page,
                               [](const PageInfo& a, const void* b) {
                                 return static_cast<const char*>(a.base) < b;  // 待修正
                               });
    pages_.insert(it, {page, pageSize});
  }
  ...
}
```

`page_index_of` 二分查找 O(log n)：

```cpp
bool TieredMemoryPool::page_index_of(const void* ptr, std::size_t& idx) const {
  std::lock_guard lock(pagesMutex_);
  // 找到第一个 base + pageSize > ptr 的页面
  auto it = std::lower_bound(pages_.begin(), pages_.end(), ptr,
                             [](const PageInfo& page, const void* p) {
                               return static_cast<const char*>(page.base) + page.pageSize <= p;
                             });
  if (it != pages_.end() && ptr >= it->base &&
      ptr < static_cast<const char*>(it->base) + it->pageSize) {
    idx = static_cast<std::size_t>(std::distance(pages_.begin(), it));
    return true;
  }
  return false;
}
```

#### 1.4 `try_merge_in_list` 简化循环逻辑

当前双重 `while` 遍历 O(n²)，且边遍历边修改链表。改为单层遍历 + 只处理第一个发现的合并对：

```cpp
bool TieredMemoryPool::try_merge_in_list(FreeNode*& list, std::size_t& count,
                                         std::size_t blockSize, FreeNode*& outMerged) {
  if (list == nullptr || list->next == nullptr) {
    return false;
  }
  // 单层遍历：只检查相邻节点对（前驱+后继）
  FreeNode** prev = &list;
  while (*prev != nullptr && (*prev)->next != nullptr) {
    FreeNode* a = *prev;
    FreeNode* b = a->next;

    void* lower = a;
    void* higher = b;
    if (higher < lower) {
      std::swap(lower, higher);
    }

    std::size_t lowerIdx = 0;
    std::size_t higherIdx = 0;
    if (page_index_of(lower, lowerIdx) && page_index_of(higher, higherIdx) &&
        lowerIdx == higherIdx &&
        static_cast<char*>(lower) + blockSize == static_cast<char*>(higher)) {
      // 移除 a 和 b
      *prev = b->next;
      count -= 2;
      outMerged = static_cast<FreeNode*>(lower);
      outMerged->next = nullptr;
      return true;
    }
    prev = &((*prev)->next);
  }
  return false;
}
```

### 验证方法

1. `xmake run bench_bench_memory_pool` 完整运行不再卡死
2. `BM_MemoryPool_BatchAllocate` 三组参数（32/100、512/100、4096/100）全部通过
3. `BM_MemoryPool_MixedSizes` 不受影响

---

## Bug 2：`LockFreeThreadPool` 高负载超时

### 现象

- `BM_LockFreeThreadPool_Tasks/10000`（4 线程池提交 10000 个空任务）超时
- `BM_LockFreeThreadPool_HeavyTask`（1000 个含计算任务）超时
- `BM_LockFreeThreadPool_Tasks/100` 和 `/1000` 可正常运行
- `LockedThreadPool` 对应基准全部正常

### 根因分析

问题出在 `LockFreeQueue::try_pop` 的状态自旋等待（`net/thread-pool/include/lock_free_queue.hpp:185-207`）：

```cpp
bool LockFreeQueue<T, Capacity, Allocator>::try_pop(T& item) {
  ...
  if (head_.compare_exchange_weak(current_head, next_head, ...)) {
    ...
    // ◆ 没有 yield() 的忙等循环！
    while (state_[head_index].load(std::memory_order_acquire) != State::PUSHED) {
      if (state_[head_index].load(std::memory_order_acquire) == State::ABORTING) {
        ...
      }
      // ◆ 缺失 yield()
    }
    ...
  }
  ...
}
```

**卡死场景**（10k 任务 / 4 worker）：

```
时间线：
  Worker-1:  CAS head 成功 → 拥有 slot 5 → state[5] = EMPTY（enqueue 还未写入完毕）
                                          ↓
  enqueue 线程:  CAS tail → 写 element → state[5] = PUSHED
                                          ↓
  Worker-1:  while(state[5] != PUSHED) 无限循环 → 100% CPU
                                          ↓
  Worker-2/3/4: 也在做 try_pop → 全部 busy-wait
                                          ↓
  整体吞吐崩溃，任务堆积，pending_ 永远 > 0 → 永不退出
```

`pop()` 方法（第 154-182 行）在 `while(empty())` 处有 yield，但在 state 自旋处同样缺失 yield，存在相同问题。

### 改进方案

#### 2.1 state 自旋等待加 `yield()`

**文件：** `net/thread-pool/include/lock_free_queue.hpp`

两处 state 自旋统一添加 `std::this_thread::yield()`：

```cpp
// pop() 中（约第 166 行）
while (state_[head_index].load(std::memory_order_acquire) != State::PUSHED) {
  if (state_[head_index].load(std::memory_order_acquire) == State::ABORTING) {
    state_[head_index].store(State::EMPTY, std::memory_order_release);
    return false;
  }
  std::this_thread::yield();  // ← 添加
}

// try_pop() 中（约第 192 行）
while (state_[head_index].load(std::memory_order_acquire) != State::PUSHED) {
  if (state_[head_index].load(std::memory_order_acquire) == State::ABORTING) {
    state_[head_index].store(State::EMPTY, std::memory_order_release);
    return false;
  }
  std::this_thread::yield();  // ← 添加
}
```

#### 2.2 `try_pop` 外层添加 `empty()` 预检查

当前 `try_pop` 缺少 `pop` 中的 `while(empty()) yield()` 守护，导致 CAS 频繁空失败：

```cpp
bool LockFreeQueue<T, Capacity, Allocator>::try_pop(T& item) {
  // 预检查：队列为空直接返回
  if (get_index(head_.load(std::memory_order_acquire)) ==
      get_index(tail_.load(std::memory_order_acquire))) {
    return false;
  }
  ...
}
```

#### 2.3 worker 批量消费（可选优化）

在 `LockFreeThreadPool::worker` 的 `try_pop` 成功后，不立即 `break`，而是继续尝试消费更多任务：

```cpp
void LockFreeThreadPool::worker(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    MoveOnlyFunction task;
    {
      std::unique_lock lock(cv_mutex_);
      cv_.wait(lock, [this, &stop_token] {
        return stop_token.stop_requested() || pending_.load(std::memory_order_acquire) > 0;
      });
    }
    if (stop_token.stop_requested()) break;

    // 批量消费：一次唤醒处理多个任务
    while (!stop_token.stop_requested()) {
      if (tasks_.try_pop(task)) {
        task();
        if (pending_.fetch_sub(1, std::memory_order_release) == 1) {
          cv_.notify_one();
        }
      } else {
        break;
      }
    }
  }
}
```

### 验证方法

1. `xmake run bench_bench_thread_pool` 全部参数通过（含 10000 和 HeavyTask）
2. `BM_LockFreeThreadPool_Tasks/10000` 不再超时，items_per_second 与 `LockedThreadPool` 可比
3. `BM_LockFreeThreadPool_HeavyTask` 正常完成

---

## 回归验证清单

| 步骤 | 命令 |
|------|------|
| 编译 | `xmake f -c && xmake build -b bench_bench_memory_pool -b bench_bench_thread_pool` |
| 内存池基准 | `xmake run bench_bench_memory_pool` |
| 线程池基准 | `xmake run bench_bench_thread_pool` |
| 完整微基准 | `bash scripts/benchmark.sh micro` |
| 测试 | `xmake test` |
| clang-tidy | `bash scripts/lint.sh` |
| cppcheck | `bash scripts/lint.sh` |
