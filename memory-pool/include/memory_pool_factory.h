#pragma once

#include <memory>

#include "i_memory_pool.h"

namespace hps {

auto CreateMemoryPool() -> std::unique_ptr<IMemoryPool>;

}  // namespace hps
