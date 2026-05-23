#pragma once
#include <cstddef>
#include <vector>

// #include "logger.h"

class MemoryPool {
public:
  MemoryPool(std::size_t blockSize, std::size_t blockCount, std::size_t pageSize = 8192);
  ~MemoryPool() noexcept;
  //
  void *allocate();
  void deallocate(void *ptr);

  auto expand() -> void;

private:
  struct FreeBlock {
    FreeBlock *next;
  };

  inline std::size_t alignSize(std::size_t size, std::size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
  }

  std::size_t blockSize_;
  std::size_t blockCount_;
  std::size_t pageSize_;
  FreeBlock *freeList_;
  std::vector<void *> pages_;
};
