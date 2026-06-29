#pragma once

#include <cstddef>
#include <cstdint>

namespace hps {

inline constexpr std::size_t kMinBlockSize = 8;
inline constexpr std::size_t kMaxBlockSize = 4096;
inline constexpr std::size_t kSizeClassCount = 10;
inline constexpr std::size_t kMaxBlocksPerClass = 32;
inline constexpr std::size_t kBatchSize = 8;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,hicpp-avoid-c-arrays)
inline constexpr std::size_t kSizeClasses[kSizeClassCount] = {8,   16,  32,   64,   128,
                                                              256, 512, 1024, 2048, 4096};

inline std::size_t round_up_size(std::size_t size) {
  if (size <= kMinBlockSize)
    return kMinBlockSize;
  if (size >= kMaxBlockSize)
    return kMaxBlockSize;
  std::size_t align = kMinBlockSize;
  while (align < size)
    align <<= 1U;
  return align;
}

inline std::size_t size_class_index(std::size_t size) {
  if (size <= kMinBlockSize)
    return 0;
  std::size_t idx = 0;
  std::size_t align = kMinBlockSize;
  while (align < size) {
    align <<= 1U;
    ++idx;
  }
  return (idx < kSizeClassCount) ? idx : (kSizeClassCount - 1);
}

inline std::size_t size_class_size(std::size_t index) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  return (index < kSizeClassCount) ? kSizeClasses[index] : kMaxBlockSize;
}

}  // namespace hps
