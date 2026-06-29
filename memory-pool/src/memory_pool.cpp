#include "memory_pool.h"

#include <cstdlib>

#include "logger.h"

namespace hps {

MemoryPool::MemoryPool() = default;

void MemoryPool::initialize(std::size_t blockSize, std::size_t blockCount) {
  std::call_once(initFlag_, [this, blockCount, blockSize]() {
    blockSize_ = alignSize(blockSize, alignof(std::max_align_t));
    blockCount_ = blockCount;
    pageSize_ = blockSize_ * blockCount_;
    expand();
    Logger::_info("内存池初始化成功，每个块大小为: " + std::to_string(blockSize_) +
                  ", 每个内存页的块数为: " + std::to_string(blockCount_));
  });
}

MemoryPool::~MemoryPool() noexcept {
  for (void *page : pages_) {
    ::operator delete(page);
  }
}

auto MemoryPool::expand() -> void {
  pageSize_ = blockSize_ * blockCount_;

  auto *page = ::operator new(pageSize_);
  pages_.push_back(page);

  auto *char_page = static_cast<char *>(page);
  for (std::size_t i = 0; i < blockCount_; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto *freeBlock = reinterpret_cast<FreeBlock *>(char_page + i * blockSize_);
    freeBlock->next = freeList_;
    freeList_ = freeBlock;
  }
}

void *MemoryPool::allocate() {
  std::lock_guard lock(mutex_);
  if (freeList_ == nullptr) {
    expand();
  }
  auto *block = freeList_;
  freeList_ = block->next;
  return static_cast<void *>(block);
}

void MemoryPool::deallocate(void *ptr) {
  if (ptr == nullptr)
    return;
  std::lock_guard lock(mutex_);
  auto *block = static_cast<FreeBlock *>(ptr);
  block->next = freeList_;
  freeList_ = block;
}

}  // namespace hps
