#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "memory_pool_base.h"
#include "size_class.h"

namespace hps {

class TieredMemoryPool : public MemoryPoolBase<TieredMemoryPool> {
public:
  TieredMemoryPool() = default;
  ~TieredMemoryPool() noexcept override;

  TieredMemoryPool(const TieredMemoryPool&) = delete;
  TieredMemoryPool& operator=(const TieredMemoryPool&) = delete;

  // CRTP 实现（非虚，编译期内联）
  void* allocate_impl(std::size_t size);
  void deallocate_impl(void* ptr, std::size_t size);

private:
  struct FreeNode {
    FreeNode* next;
  };

  struct ThreadCache {
    std::array<FreeNode*, kSizeClassCount> freeLists_{};
    std::array<std::size_t, kSizeClassCount> counts_{};
    ThreadCache* next_{nullptr};  // 侵入式注册链表
    TieredMemoryPool* owner_{nullptr};
  };

  struct PageInfo {
    void* base;
    std::size_t pageSize;
  };

  static thread_local ThreadCache tl_cache_;

  void ensure_registered(ThreadCache& cache);
  void central_allocate_batch(std::size_t classIdx);
  void return_batch_to_central(std::size_t classIdx, FreeNode* head, std::size_t count);
  void try_merge_and_promote(ThreadCache& cache, std::size_t classIdx);
  void try_merge_central(std::size_t classIdx);
  bool try_merge_in_list(FreeNode*& list, std::size_t& count, std::size_t blockSize,
                         FreeNode*& outMerged);
  // 返回页索引；未找到返回 false，idx 不定义
  bool page_index_of(const void* ptr, std::size_t& idx) const;

  std::mutex centralMutex_;
  mutable std::mutex pagesMutex_;
  std::array<FreeNode*, kSizeClassCount> centralFreeLists_{};
  std::vector<PageInfo> pages_;
  std::atomic<bool> destroyed_{false};

  // ThreadCache 注册表（H6：解决 thread_local 生命周期与池解耦导致的悬挂指针）
  std::mutex registryMutex_;
  ThreadCache* registryHead_{nullptr};
};

}  // namespace hps
