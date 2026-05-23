#include "memory_pool.h"

#include <cstdlib>

MemoryPool::MemoryPool(std::size_t blockSize, std::size_t blockCount, std::size_t pageSize)
    : blockSize_(alignSize(blockSize, alignof(std::max_align_t))),
      blockCount_(blockCount),
      freeList_(nullptr),
      pageSize_(pageSize) {
  expand();
}

MemoryPool::~MemoryPool() noexcept {
  for (void *page : pages_) {
    ::operator delete(page);
  }
}

auto MemoryPool::expand() -> void {
  if (pageSize_ < blockSize_ * blockCount_) {
    pageSize_ = blockSize_ * blockCount_;
  } else {
    blockCount_ = pageSize_ / blockSize_;
  }
  void *page = ::operator new(pageSize_);
  pages_.push_back(page);

  char *block = static_cast<char *>(page);
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
  return block;
}

void MemoryPool::deallocate(void *ptr) {
  if (!ptr)
    return;
  FreeBlock *block = static_cast<FreeBlock *>(ptr);
  block->next = freeList_;
  freeList_ = block;
}
