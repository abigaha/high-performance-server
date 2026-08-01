#include "pending_chunk_deletions.h"

#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <string>

namespace hps {
namespace {

const std::string kPhysicalDeletionError = "physical chunk deletion failed";
const std::string kReferenceLookupError = "chunk reference lookup failed";
const std::string kCompletionError = "pending chunk deletion completion failed";
const std::string kCancellationError = "pending chunk deletion cancellation failed";

using ReleaseFn = std::function<MutationResult<std::monostate>(const PendingChunkDeletion&, const std::string&)>;
using AbortFn = std::function<
  MutationResult<std::size_t>(std::size_t, MutationStatus, std::optional<std::string>, const std::string&)>;
using AbortAfterReleaseFn =
  std::function<MutationResult<std::size_t>(std::size_t, const MutationResult<std::monostate>&, const std::string&)>;

// 处理单条 PendingChunkDeletion 记录；返回 nullopt 表示继续，返回 MutationResult 表示提前结束整批。
std::optional<MutationResult<std::size_t>> process_one_deletion(IDatabasePool& database,
                                                                IFileSystem& file_system,
                                                                const PendingChunkDeletion& deletion,
                                                                std::size_t index,
                                                                std::size_t& processed,
                                                                const ReleaseFn& release_claim,
                                                                const AbortFn& abort_batch,
                                                                const AbortAfterReleaseFn& abort_after_release) {
  LookupResult<bool> references{LookupStatus::STORAGE_ERROR, std::nullopt};
  try {
    references = database.has_chunk_references(deletion.chunk_hash);
  } catch (...) {
    return abort_batch(index,
                       MutationStatus::STORAGE_ERROR,
                       std::optional<std::string>{"CHUNK_REFERENCE_LOOKUP_FAILED"},
                       kReferenceLookupError);
  }
  if (references.status != LookupStatus::FOUND || !references.value) {
    const auto status = references.status == LookupStatus::INVALID_DATA ? MutationStatus::INVALID_STATE
                                                                        : MutationStatus::STORAGE_ERROR;
    return abort_batch(index,
                       status,
                       std::optional<std::string>{"CHUNK_REFERENCE_LOOKUP_FAILED"},
                       kReferenceLookupError);
  }
  if (*references.value) {
    MutationResult<std::monostate> cancelled{MutationStatus::STORAGE_ERROR, std::nullopt, "PENDING_CANCEL_FAILED"};
    try {
      cancelled = database.cancel_pending_chunk_deletion(deletion.chunk_hash, deletion.claim_token);
    } catch (...) {
      return abort_batch(index,
                         MutationStatus::STORAGE_ERROR,
                         std::optional<std::string>{"PENDING_CANCEL_FAILED"},
                         kCancellationError);
    }
    if (cancelled.status != MutationStatus::OK) {
      return abort_batch(index, cancelled.status, cancelled.detail, kCancellationError);
    }
    ++processed;
    return std::nullopt;
  }
  ChunkDeleteStatus del_status = ChunkDeleteStatus::ERROR;
  try {
    del_status = file_system.delete_file_status("chunks/" + deletion.chunk_hash);
  } catch (...) {
    const auto released = release_claim(deletion, kPhysicalDeletionError);
    if (released.status != MutationStatus::OK) {
      return abort_after_release(index + 1, released, kPhysicalDeletionError);
    }
    ++processed;
    return std::nullopt;
  }
  if (del_status == ChunkDeleteStatus::DELETED || del_status == ChunkDeleteStatus::NOT_FOUND) {
    MutationResult<std::monostate> completed{MutationStatus::STORAGE_ERROR, std::nullopt, "PENDING_COMPLETE_FAILED"};
    try {
      completed = database.complete_pending_chunk_deletion(deletion.chunk_hash, deletion.claim_token);
    } catch (...) {
      return abort_batch(index,
                         MutationStatus::STORAGE_ERROR,
                         std::optional<std::string>{"PENDING_COMPLETE_FAILED"},
                         kCompletionError);
    }
    if (completed.status != MutationStatus::OK) {
      return abort_batch(index, completed.status, completed.detail, kCompletionError);
    }
    ++processed;
  } else {
    const auto released = release_claim(deletion, kPhysicalDeletionError);
    if (released.status != MutationStatus::OK) {
      return abort_after_release(index + 1, released, kPhysicalDeletionError);
    }
    ++processed;
  }
  return std::nullopt;
}

} // namespace

MutationResult<std::size_t> run_pending_chunk_deletions(IDatabasePool& database,
                                                        IFileSystem& file_system,
                                                        ChunkLifecycleCoordinator& coordinator,
                                                        std::size_t limit) {
  auto cleanup_guard = coordinator.acquire_cleanup_guard();
  return run_pending_chunk_deletions_guarded(database, file_system, cleanup_guard.permit(), limit);
}

MutationResult<std::size_t> run_pending_chunk_deletions_guarded(
  IDatabasePool& database,
  IFileSystem& file_system,
  const ChunkLifecycleCoordinator::CleanupPermit& cleanup_permit,
  std::size_t limit) {
  if (!database.accepts_cleanup_permit(cleanup_permit)) {
    return {MutationStatus::INVALID_STATE, std::nullopt, "CLEANUP_PERMIT_INVALID"};
  }
  const auto stale_before = std::chrono::system_clock::now() - std::chrono::minutes(10);
  MutationResult<std::vector<PendingChunkDeletion>> claimed{MutationStatus::STORAGE_ERROR,
                                                            std::nullopt,
                                                            "PENDING_CLAIM_FAILED"};
  try {
    claimed = database.claim_pending_chunk_deletions(limit, stale_before);
  } catch (...) {
    return {MutationStatus::STORAGE_ERROR, std::nullopt, "PENDING_CLAIM_FAILED"};
  }
  if (claimed.status != MutationStatus::OK || !claimed.value) {
    return {claimed.status, std::nullopt, claimed.detail};
  }

  const auto& deletions = *claimed.value;
  std::size_t processed = 0;

  const ReleaseFn release_claim = [&](const PendingChunkDeletion& deletion, const std::string& error) {
    try {
      return database.release_pending_chunk_deletion(deletion.chunk_hash, deletion.claim_token, error);
    } catch (...) {
      return MutationResult<std::monostate>{MutationStatus::STORAGE_ERROR, std::nullopt, "PENDING_RELEASE_FAILED"};
    }
  };
  const auto release_remaining_claims = [&](std::size_t first, const std::string& error) {
    std::optional<std::string> failure_detail;
    for (std::size_t i = first; i < deletions.size(); ++i) {
      const auto released = release_claim(deletions[i], error);
      if (released.status != MutationStatus::OK && !failure_detail) {
        failure_detail = released.detail.value_or("PENDING_RELEASE_FAILED");
      }
    }
    return failure_detail;
  };
  const AbortFn abort_batch = [&](std::size_t first,
                                  MutationStatus status,
                                  std::optional<std::string> detail,
                                  const std::string& error) -> MutationResult<std::size_t> {
    if (const auto release_failure = release_remaining_claims(first, error)) {
      return {MutationStatus::STORAGE_ERROR, processed, release_failure};
    }
    return {status, processed, detail};
  };
  const AbortAfterReleaseFn abort_after_release_failure = [&](std::size_t first_remaining,
                                                              const MutationResult<std::monostate>& failed_release,
                                                              const std::string& error) -> MutationResult<std::size_t> {
    static_cast<void>(release_remaining_claims(first_remaining, error));
    return {MutationStatus::STORAGE_ERROR, processed, failed_release.detail.value_or("PENDING_RELEASE_FAILED")};
  };

  for (std::size_t index = 0; index < deletions.size(); ++index) {
    auto early_exit = process_one_deletion(database,
                                           file_system,
                                           deletions[index],
                                           index,
                                           processed,
                                           release_claim,
                                           abort_batch,
                                           abort_after_release_failure);
    if (early_exit) {
      return *early_exit;
    }
  }
  return {MutationStatus::OK, processed, std::nullopt};
}

} // namespace hps
