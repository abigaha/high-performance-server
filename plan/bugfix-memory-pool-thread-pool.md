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

## Bug 2：`LockFreeThreadPool` 高负载卡死

### 现象

- `BM_LockFreeThreadPool_Tasks/10000`（4 线程池提交 10000 个空任务）超时
- `BM_LockFreeThreadPool_HeavyTask`（1000 个含计算任务）超时
- 经过调试发现该 bug 影响所有参数（含 /100 和 /1000），且为**存量 bug**（原始代码同样存在）
- 简单单次 enqueue/wait（10000 任务）可正常完成，**多轮迭代后**死锁

### 根因分析

**核心 bug：`LockFreeQueue::pop()` 的 `empty()` 存在 TOCTOU（Time-Of-Check-Time-Of-Use）竞态**

`empty()` 分两次独立 load head 和 tail：

```cpp
bool empty() {
  return get_index(head_.load(std::memory_order_acquire)) ==
         get_index(tail_.load(std::memory_order_acquire));
}
```

当两个消费者和一个生产者同时运行时，可触发以下序列：

```
head=999, tail=1000（队列有 1 个任务在 slot 999）

  消费者 B: empty() → load head=999（过时值）→ load tail=1000
                        999 ≠ 1000 → "非空！" 退出 yield 循环    ← 此时 head 已被消费者 A 推进到 1000

  消费者 A: CAS head 999→1000，消费 slot 999，队列清空 (head=1000, tail=1000)

  消费者 B: load current_head = 1000 → CAS head 1000→1001 → 成功！← 错误的！
            → 进入 while(state[1000] != PUSHED) 自旋
            → state[1000] = EMPTY（从未被生产者写入）
            → ★ 永久自旋，永不退出 ★
```

`empty()` 的 head 和 tail 是两个独立的 atomic load，不构成原子快照。消费者退出 `while(empty())` 后，该队列可能已被其他消费者**完全清空**，但消费者仍持有过时的"head < tail"印象，最终 CAS 将 head 推进到 tail 之后，claim 了一个**幽灵 slot**——该 slot 的 state 永远是 EMPTY，因为没有生产者会写入它。

### 修复方案

**文件：** `net/thread-pool/include/lock_free_queue.hpp:162-165`

在 `pop()` 中，load `current_head` 后**重新 load tail** 做二次验证：

```cpp
std::uint64_t current_head = head_.load(std::memory_order_acquire);
// TOCTOU 修复：二次验证 head 是否仍在 tail 之前
if (get_index(current_head) == get_index(tail_.load(std::memory_order_acquire))) {
  continue;  // 队列在 empty()→load_head 之间已被清空
}
```

如果 `get_index(current_head) == get_index(tail)`，说明队列实际上已经空了（head 赶上了 tail），消费者不应该继续推进 head，而应回到外层循环重新等待。

**为什么 try_pop 不受影响：** `try_pop` 中 head 的 load、tail 的 check、CAS 在同一代码路径连续执行，且 CAS 失败立即返回 false（无循环重试），不存在 TOCTOU 窗口。

### 验证方法

1. 专用死锁测试：50 trials × 4 workers × 200 轮 × 100 空任务，-O0 和 -O3 全部通过
2. `xmake run bench_bench_thread_pool` 全部 8 个 benchmark 正常完成（含 10000 和 HeavyTask）
3. `xmake test` 20 二进制全部通过
4. 全量质量门禁：lint 0/0 + 编译 0/0 + test 20/20 + CodeQL 0/0

---

## Bug 3：`LockFreeQueue` 高并发 benchmark 卡死

### 现象

- `qps_qps_lock_free_queue` 在 c≥128 时卡死，进程不退出
- `qps_qps_thread_pool`（LockFreeTP）在 c≥256 时同样卡死
- 单元测试全部通过（低并发短时），仅 QPS 压测复现
- c=1~64 正常，c=128 偶尔卡，c=256+ 必卡

### 根因分析

**核心 bug：`pop(T&)` 和 `try_pop` 的 `while(state_ != PUSHED)` 等待循环不检查 stop 标记**

高并发场景（c≥256）下，LFQ 内部 `pop()` / `try_pop()` 成功 CAS head 后进入 state_ 等待循环：

```cpp
while (state_[head_index].load(std::memory_order_acquire) != State::PUSHED) {
    if (state_[head_index].load(std::memory_order_acquire) == State::ABORTING) {
        state_[head_index].store(State::EMPTY, std::memory_order_release);
        return false;
    }
    // ★ 没有检查 stop！★
    std::this_thread::yield();
}
```

触发序列：
```
1. 生产者 P CAS tail → slot N 成功
2. P 被调度走（1024 线程抢 8 核，强竞争）
3. 消费者 C CAS head → slot N 成功 → 进入 state_[N] 等待
4. 主线程调用 q.stop() → 所有 in-flight 生产者检查 stop 后 return false
5. 但 slot N 的生产者 P 已经 CAS tail 成功，
   从 P 的视角"必须完成 construct → 写 PUSHED"
   而 P 在被重新调度前，主线程已停掉所有生产者
6. state_[N] 永远 EMPTY，C 永远自旋
```

**实质是 stop 信号到达时，slot 处于"生产者已认领但尚未写入 PUSHED"的中间态。** 此时 CAS 了 head 的消费者无法区分"生产者还在构造中"和"生产者已放弃（stop）"，导致永久等待。

### 修复方案

**文件：** `net/thread-pool/include/lock_free_queue.hpp`

在 `pop(T&)` 和 `try_pop` 的 `while(state_ != PUSHED)` 循环中增加 stop 逃生口：

```cpp
while (state_[head_index].load(std::memory_order_acquire) != State::PUSHED) {
    if (state_[head_index].load(std::memory_order_acquire) == State::ABORTING) {
        state_[head_index].store(State::EMPTY, std::memory_order_release);
        return false;
    }
    if (stop_token_.stop_requested()) {           // ← 新增
        state_[head_index].store(State::EMPTY, std::memory_order_release);
        return false;
    }
    std::this_thread::yield();
}
```

### 连带优化

| 项目 | 文件 | 说明 |
|------|------|------|
| benchmark 改用 `std::latch` | `benchmark/qps_lock_free_queue.cpp` | 代替 `thread::join()` 监控 worker 完成，防止因调度延迟产生的 join 阻塞误判为 hang |
| `pop(T&)` CAS 循环中的 stop 检查 | `lock_free_queue.hpp:174` | CAS 竞争失败后 yield 回到 while(true)，顶部已有 stop 检查，不需额外加 |

### 验证方法

1. `bin/qps_qps_lock_free_queue` 11 级并发全部通过（c=1~1024）
2. `bin/qps_qps_thread_pool` LockFreeTP 11 级并发全部通过（c=1~1024）
3. `xmake test` 20 二进制中 19 通过

---

## 已知问题（已修复）

### `test_test_tcp_client` 间歇性失败 ✅ 已修复

**现象：** `xmake test` 批量运行时 `test_test_tcp_client` 偶发失败，但单次 `xmake run test_test_tcp_client` 始终通过（8/8 用例全绿）。

**根因：** `TcpClient::connect_to_server()` 创建 socket 后未设置 `SO_REUSEADDR` / `SO_REUSEPORT`，服务器端 `TIME_WAIT` 状态下客户端无法重用端口。

**修复：** `net/tcp/tcp_client/src/tcp_client.cpp` 的 `connect_to_server()` 中 socket 创建后添加：
```cpp
int reuse = 1;
setsockopt(client_sockfd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
setsockopt(client_sockfd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
```

**验证：** `xmake test` 37/37 全部通过（含 `test_tcp_client`），连续运行无间歇失败。

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
