#pragma once
#include <cstddef>
#include <mutex>
#include <vector>

namespace hps {

class MemoryPool {
public:
  MemoryPool();
  ~MemoryPool() noexcept;

  void initialize(std::size_t blockSize, std::size_t blockCount);

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

  std::size_t blockSize_{};
  std::size_t blockCount_{};
  std::size_t pageSize_{};
  FreeBlock *freeList_{nullptr};
  std::vector<void *> pages_;
  std::once_flag initFlag_;
};

}  // namespace hps
