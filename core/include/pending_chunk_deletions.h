#pragma once

#include "chunk_lifecycle_coordinator.h"
#include "i_file_system.h"
#include "idatabase_pool.h"

#include <cstddef>

namespace hps {

MutationResult<std::size_t> run_pending_chunk_deletions(IDatabasePool& database,
                                                        IFileSystem& file_system,
                                                        ChunkLifecycleCoordinator& coordinator,
                                                        std::size_t limit);

MutationResult<std::size_t> run_pending_chunk_deletions_guarded(
  IDatabasePool& database,
  IFileSystem& file_system,
  const ChunkLifecycleCoordinator::CleanupPermit& cleanup_permit,
  std::size_t limit);

} // namespace hps
