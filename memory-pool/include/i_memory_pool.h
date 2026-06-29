#pragma once

#include <cstddef>
#include <memory>

namespace hps {

class IMemoryPool {
public:
  virtual ~IMemoryPool() = default;

  virtual void* allocate(std::size_t size) = 0;
  virtual void deallocate(void* ptr, std::size_t size) = 0;
};

}  // namespace hps
