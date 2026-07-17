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

  registryHead_ = nullptr;

  tl_cache_ = ThreadCache{};

  std::lock_guard plock(pagesMutex_);
  for (auto& page : pages_) {
    ::operator delete(page.base);
  }
  pages_.clear();
}

void TieredMemoryPool::ensure_registered(ThreadCache& cache) {
  if (cache.owner_ == this) {
    return;  // 已注册
  }
  std::lock_guard lock(registryMutex_);
  if (cache.owner_ == this) {
    return;  // double-check
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
    auto it = std::lower_bound(
        pages_.begin(), pages_.end(), page,
        [](const PageInfo& a, const void* b) { return static_cast<const char*>(a.base) < b; });
    pages_.insert(it, {page, pageSize});
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

bool TieredMemoryPool::try_merge_in_list(FreeNode*& list, std::size_t& count, std::size_t blockSize,
                                         FreeNode*& outMerged) {
  if (list == nullptr || list->next == nullptr) {
    return false;
  }

  // 快慢指针循环检测（防御性，防止链表成环导致死循环）
  auto* slow = list;
  auto* fast = list;
  while (fast != nullptr && fast->next != nullptr) {
    slow = slow->next;
    fast = fast->next->next;
    if (slow == fast) {
      return false;
    }
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

bool TieredMemoryPool::page_index_of(const void* ptr, std::size_t& idx) const {
  std::lock_guard lock(pagesMutex_);
  // 二分查找：找到第一个 base + pageSize > ptr 的页面
  auto it =
      std::lower_bound(pages_.begin(), pages_.end(), ptr, [](const PageInfo& page, const void* p) {
        return static_cast<const char*>(page.base) + page.pageSize <= p;
      });
  if (it != pages_.end() && ptr >= it->base &&
      ptr < static_cast<const char*>(it->base) + it->pageSize) {
    idx = static_cast<std::size_t>(std::distance(pages_.begin(), it));
    return true;
  }
  return false;
}

}  // namespace hps
