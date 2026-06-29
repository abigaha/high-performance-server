#include "tiered_memory_pool.h"

#include <cstdlib>
#include <new>
#include <span>

#include "logger.h"

namespace hps {

thread_local TieredMemoryPool::ThreadCache TieredMemoryPool::tl_cache_;

TieredMemoryPool::~TieredMemoryPool() noexcept {
  destroyed_.store(true, std::memory_order_release);

  tl_cache_ = ThreadCache{};

  for (auto& page : pages_) {
    ::operator delete(page.base);
  }
}

void* TieredMemoryPool::allocate(std::size_t size) {
  if (destroyed_.load(std::memory_order_acquire)) [[unlikely]] {
    return ::operator new(size);
  }

  auto const classIdx = size_class_index(size);
  auto& cache = tl_cache_;

  if (cache.freeLists_[classIdx] != nullptr) {
    auto* node = cache.freeLists_[classIdx];
    cache.freeLists_[classIdx] = node->next;
    cache.counts_[classIdx]--;
    return static_cast<void*>(node);
  }

  {
    std::lock_guard lock(centralMutex_);
    if (centralFreeLists_[classIdx] == nullptr) {
      central_allocate_batch(classIdx);
    }

    auto* batch = centralFreeLists_[classIdx];
    auto* node = batch;
    auto* remaining = node->next;

    std::size_t taken = 1;
    cache.freeLists_[classIdx] = remaining;
    cache.counts_[classIdx] = 0;

    auto* walk = remaining;
    while (walk != nullptr && taken < kBatchSize) {
      walk = walk->next;
      taken++;
    }

    if (walk != nullptr) {
      auto* nextBatch = walk->next;
      walk->next = nullptr;
      cache.freeLists_[classIdx] = remaining;
      cache.counts_[classIdx] = taken - 1;
      centralFreeLists_[classIdx] = nextBatch;
    } else {
      cache.freeLists_[classIdx] = remaining;
      cache.counts_[classIdx] = taken - 1;
      centralFreeLists_[classIdx] = nullptr;
    }

    return static_cast<void*>(node);
  }
}

void TieredMemoryPool::deallocate(void* ptr, std::size_t size) {
  if (ptr == nullptr) {
    return;
  }

  if (destroyed_.load(std::memory_order_acquire)) [[unlikely]] {
    ::operator delete(ptr);
    return;
  }

  auto const classIdx = size_class_index(size);
  auto& cache = tl_cache_;

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

      if (static_cast<char*>(lower) + blockSize == static_cast<char*>(higher) &&
          page_index_of(lower) == page_index_of(higher)) {
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

std::size_t TieredMemoryPool::page_index_of(const void* ptr) const {
  std::lock_guard lock(pagesMutex_);
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    auto pageSpan = std::span(static_cast<const char*>(pages_[i].base), pages_[i].pageSize);
    if (ptr >= pageSpan.data() && ptr < pageSpan.data() + pageSpan.size()) {
      return i;
    }
  }
  return static_cast<std::size_t>(-1);
}

}  // namespace hps
