#include "memory_pool.h"

#include <cstdlib>
// #include <memory>

namespace hps {

MemoryPool::MemoryPool() : blockSize_{0}, blockCount_{0}, pageSize_(0), freeList_(nullptr) {}

void MemoryPool::initialize(std::size_t blockSize, std::size_t blockCount) {
  std::call_once(initFlag_, [this, blockCount, blockSize]() {
    blockSize_ = alignSize(blockSize, alignof(std::max_align_t));
    blockCount_ = blockCount;
    pageSize_ = blockSize_ * blockCount_;
    expand();
  });
}

MemoryPool::~MemoryPool() noexcept {
  for (void *page : pages_) {
    ::operator delete(page);
  }
}

auto MemoryPool::expand() -> void {
  pageSize_ = blockSize_ * blockCount_;

  void *page = ::operator new(pageSize_);
  pages_.push_back(page);

  char *block = reinterpret_cast<char *>(page);
  for (std::size_t i = 0; i < blockCount_; ++i) {
    FreeBlock *freeBlock = reinterpret_cast<FreeBlock *>(block);
    freeBlock->next = freeList_;
    freeList_ = freeBlock;
    block += blockSize_;
  }
}

void *MemoryPool::allocate() {
  if (!freeList_) {
    expand();
  }
  FreeBlock *block = freeList_;
  freeList_ = block->next;
  return reinterpret_cast<void *>(block);
}

void MemoryPool::deallocate(void *ptr) {
  if (!ptr)
    return;
  FreeBlock *block = reinterpret_cast<FreeBlock *>(ptr);
  block->next = freeList_;
  freeList_ = block;
}

}  // namespace hps
