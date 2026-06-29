#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "i_memory_pool.h"
#include "size_class.h"

namespace hps {

class TieredMemoryPool : public IMemoryPool {
public:
  TieredMemoryPool() = default;
  ~TieredMemoryPool() noexcept override;

  void* allocate(std::size_t size) override;
  void deallocate(void* ptr, std::size_t size) override;

private:
  struct FreeNode {
    FreeNode* next;
  };

  struct ThreadCache {
    std::array<FreeNode*, kSizeClassCount> freeLists_{};
    std::array<std::size_t, kSizeClassCount> counts_{};
  };

  struct PageInfo {
    void* base;
    std::size_t pageSize;
  };

  static thread_local ThreadCache tl_cache_;

  void central_allocate_batch(std::size_t classIdx);
  void return_batch_to_central(std::size_t classIdx, FreeNode* head, std::size_t count);
  void try_merge_and_promote(ThreadCache& cache, std::size_t classIdx);
  void try_merge_central(std::size_t classIdx);
  bool try_merge_in_list(FreeNode*& list, std::size_t& count, std::size_t blockSize,
                         FreeNode*& outMerged);
  std::size_t page_index_of(const void* ptr) const;

  std::mutex centralMutex_;
  mutable std::mutex pagesMutex_;
  std::array<FreeNode*, kSizeClassCount> centralFreeLists_{};
  std::vector<PageInfo> pages_;
  std::atomic<bool> destroyed_{false};
};

}  // namespace hps
