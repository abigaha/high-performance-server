#include "tiered_memory_pool.h"

#include <algorithm>
#include <cstdlib>
#include <new>
#include <span>

#include "logger.h"

namespace hps {

thread_local TieredMemoryPool::ThreadCache TieredMemoryPool::tl_cache_;

TieredMemoryPool::~TieredMemoryPool() noexcept {
  destroyed_.store(true, std::memory_order_release);

  // H6：遍历注册表，清空所有线程的 ThreadCache，消除悬挂指针
  {
    std::lock_guard lock(registryMutex_);
    auto* cur = registryHead_;
    while (cur != nullptr) {
      cur->freeLists_.fill(nullptr);
      cur->counts_.fill(0);
      cur->owner_ = nullptr;
      cur = cur->next_;
    }
    registryHead_ = nullptr;
  }

  // 清空当前线程 cache（可能已在上面清空，幂等）
  tl_cache_ = ThreadCache{};

  std::lock_guard plock(pagesMutex_);
  for (auto& page : pages_) {
    ::operator delete(page.base);
  }
  pages_.clear();
}

void TieredMemoryPool::ensure_registered(ThreadCache& cache) {
  if (cache.owner_ == this) {
    return; // 已注册
  }
  std::lock_guard lock(registryMutex_);
  if (cache.owner_ == this) {
    return; // double-check
  }
  cache.owner_ = this;
  cache.next_ = registryHead_;
  registryHead_ = &cache;
}

void* TieredMemoryPool::allocate_impl(std::size_t size) {
  if (destroyed_.load(std::memory_order_acquire)) [[unlikely]] {
    return ::operator new(size);
  }

  auto const classIdx = size_class_index(size);
  auto& cache = tl_cache_;
  ensure_registered(cache);

  if (cache.freeLists_[classIdx] != nullptr) {
    auto* node = cache.freeLists_[classIdx];
    cache.freeLists_[classIdx] = node->next;
    cache.counts_[classIdx]--;
    return static_cast<void*>(node);
  }

  FreeNode* node = nullptr;
  {
    std::lock_guard lock(centralMutex_);
    if (centralFreeLists_[classIdx] == nullptr) {
      central_allocate_batch(classIdx);
    }

    // N8-L：简化 batch 取出逻辑
    node = centralFreeLists_[classIdx];
    auto* remaining = node->next;

    std::size_t taken = 0;
    auto* walk = remaining;
    while (walk != nullptr && taken < kBatchSize - 1) {
      walk = walk->next;
      ++taken;
    }

    if (walk != nullptr) {
      centralFreeLists_[classIdx] = walk->next;
      walk->next = nullptr;
    } else {
      centralFreeLists_[classIdx] = nullptr;
    }
    cache.freeLists_[classIdx] = remaining;
    cache.counts_[classIdx] = taken;
  }

  return static_cast<void*>(node);
}

void TieredMemoryPool::deallocate_impl(void* ptr, std::size_t size) {
  if (ptr == nullptr) {
    return;
  }

  if (destroyed_.load(std::memory_order_acquire)) [[unlikely]] {
    ::operator delete(ptr);
    return;
  }

  auto const classIdx = size_class_index(size);
  auto& cache = tl_cache_;
  ensure_registered(cache);

  auto* node = static_cast<FreeNode*>(ptr);
  node->next = cache.freeLists_[classIdx];
  cache.freeLists_[classIdx] = node;
  cache.counts_[classIdx]++;

  if (cache.counts_[classIdx] > kMaxBlocksPerClass) {
    try_merge_and_promote(cache, classIdx);

    if (cache.counts_[classIdx] > kMaxBlocksPerClass) {
      auto excess = cache.counts_[classIdx] - kMaxBlocksPerClass;
      if (excess > kBatchSize) {
        excess = kBatchSize;
      }

      auto* head = cache.freeLists_[classIdx];
      auto* split = head;
      for (std::size_t i = 1; i < excess && split != nullptr; ++i) {
        split = split->next;
      }
      if (split != nullptr) {
        auto* batch = head;
        auto* rest = split->next;
        split->next = nullptr;
        cache.freeLists_[classIdx] = rest;
        cache.counts_[classIdx] -= excess;

        return_batch_to_central(classIdx, batch, excess);
      }
    }
  }
}

void TieredMemoryPool::central_allocate_batch(std::size_t classIdx) {
  auto const blockSize = size_class_size(classIdx);
  auto const pageSize = blockSize * kMaxBlocksPerClass;
  auto* page = ::operator new(pageSize);
  {
    std::lock_guard lock(pagesMutex_);
    pages_.push_back({page, pageSize});
  }

  auto const count = pageSize / blockSize;

  FreeNode* head = nullptr;
  for (std::size_t i = 0; i < count; ++i) {
    auto* block = reinterpret_cast<FreeNode*>(
        &(std::span(static_cast<char*>(page), pageSize)[i * blockSize]));
    block->next = head;
    head = block;
  }

  centralFreeLists_[classIdx] = head;
}

void TieredMemoryPool::return_batch_to_central(std::size_t classIdx, FreeNode* head,
                                               std::size_t count) {
  std::lock_guard lock(centralMutex_);

  auto* walk = head;
  while (walk != nullptr && walk->next != nullptr) {
    walk = walk->next;
  }
  if (walk != nullptr) {
    walk->next = centralFreeLists_[classIdx];
    centralFreeLists_[classIdx] = head;
  }

  std::size_t total = count;
  auto* cur = centralFreeLists_[classIdx];
  while (cur != nullptr) {
    total++;
    cur = cur->next;
  }

  if (total > kMaxBlocksPerClass * 2) {
    try_merge_central(classIdx);
  }
}

// NOLINTNEXTLINE(misc-no-recursion)
void TieredMemoryPool::try_merge_and_promote(ThreadCache& cache, std::size_t classIdx) {
  if (classIdx >= kSizeClassCount - 1) {
    return;
  }

  FreeNode* merged = nullptr;
  while (try_merge_in_list(cache.freeLists_[classIdx], cache.counts_[classIdx],
                           size_class_size(classIdx), merged)) {
    auto const nextIdx = classIdx + 1;
    merged->next = cache.freeLists_[nextIdx];
    cache.freeLists_[nextIdx] = merged;
    cache.counts_[nextIdx]++;

    if (cache.counts_[nextIdx] > kMaxBlocksPerClass) {
      try_merge_and_promote(cache, nextIdx);
    }

    if (cache.counts_[classIdx] <= kMaxBlocksPerClass) {
      break;
    }
  }
}

bool TieredMemoryPool::try_merge_in_list(FreeNode*& list, std::size_t& count, std::size_t blockSize,
                                         FreeNode*& outMerged) {
  if (list == nullptr || list->next == nullptr) {
    return false;
  }

  auto* prev = &list;
  while (*prev != nullptr && (*prev)->next != nullptr) {
    auto* a = *prev;

    auto* innerPrev = prev;
    while (*innerPrev != nullptr && (*innerPrev)->next != nullptr) {
      auto* b = *innerPrev;
      if (a == b) {
        innerPrev = &((*innerPrev)->next);
        continue;
      }

      void* lower = a;
      void* higher = b;

      if (static_cast<char*>(higher) < static_cast<char*>(lower)) {
        std::swap(lower, higher);
      }

      std::size_t lowerIdx = 0;
      std::size_t higherIdx = 0;
      // N7-L：显式检查 page_index_of 返回值，避免 size_t(-1)==size_t(-1) 误判同页
      if (page_index_of(lower, lowerIdx) && page_index_of(higher, higherIdx) &&
          lowerIdx == higherIdx &&
          static_cast<char*>(lower) + blockSize == static_cast<char*>(higher)) {
        *prev = a->next;
        *innerPrev = b->next;
        count -= 2;

        outMerged = static_cast<FreeNode*>(lower);
        outMerged->next = nullptr;

        return true;
      }

      innerPrev = &((*innerPrev)->next);
    }

    prev = &((*prev)->next);
  }

  return false;
}

// NOLINTNEXTLINE(misc-no-recursion)
void TieredMemoryPool::try_merge_central(std::size_t classIdx) {
  if (classIdx >= kSizeClassCount - 1) {
    return;
  }

  auto& list = centralFreeLists_[classIdx];
  std::size_t total = 0;
  auto* cur = list;
  while (cur != nullptr) {
    total++;
    cur = cur->next;
  }

  FreeNode* merged = nullptr;
  while (total > kMaxBlocksPerClass * 2) {
    if (!try_merge_in_list(list, total, size_class_size(classIdx), merged)) {
      break;
    }

    auto const nextIdx = classIdx + 1;
    merged->next = centralFreeLists_[nextIdx];
    centralFreeLists_[nextIdx] = merged;

    std::size_t nextTotal = 0;
    cur = centralFreeLists_[nextIdx];
    while (cur != nullptr) {
      nextTotal++;
      cur = cur->next;
    }

    if (nextTotal > kMaxBlocksPerClass * 2) {
      try_merge_central(nextIdx);
    }
  }
}

// M2-M：page_index_of 改为线性查找但避免排序开销（pages_ 通常 <100），
//       实际优化为：先按 base 排序的辅助结构会引入额外维护成本，
//       这里保持线性但返回 bool，调用方显式检查。
// N7-L：返回 bool，未找到返回 false，消除 size_t(-1) 哨兵隐患
bool TieredMemoryPool::page_index_of(const void* ptr, std::size_t& idx) const {
  std::lock_guard lock(pagesMutex_);
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    auto pageSpan = std::span(static_cast<const char*>(pages_[i].base), pages_[i].pageSize);
    if (ptr >= static_cast<const void*>(pageSpan.data()) &&
        ptr < static_cast<const void*>(pageSpan.data() + pageSpan.size())) {
      idx = i;
      return true;
    }
  }
  return false;
}

}  // namespace hps
