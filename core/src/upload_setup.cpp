#include "upload_setup.h"

#include "i_file_system.h"
#include "idatabase_pool.h"
#include "upload_policy.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace hps {

HttpServer::UploadStreamSetup make_upload_setup(const IDatabasePool& database,
                                                IFileSystem& file_system,
                                                ChunkLifecycleCoordinator& coordinator) {
  if (!database.is_chunk_lifecycle_coordinator_bound_to(coordinator)) {
    throw std::logic_error("upload setup coordinator is not the database canonical coordinator");
  }

  return [&file_system, &coordinator](const HttpRequest&, UploadStreamContext& context, HttpParser&) {
    context.chunk_lifecycle_guard = coordinator.acquire_upload_guard();
    context.store_chunk_data = [&file_system](std::string_view data, const std::string& chunk_hash) {
      const std::vector<char> buffer(data.begin(), data.end());
      return file_system.store_file("chunks/" + chunk_hash, buffer);
    };
    const auto file_name = context.file_name;
    context.set_initial_chunk_probe(kAudioSignatureProbeSize,
                                    [file_name](std::string_view prefix) -> std::optional<HttpResponse> {
                                      const auto validation =
                                        validate_audio_signature(file_name, AudioSignaturePrefix{prefix});
                                      if (validation.accepted) {
                                        return std::nullopt;
                                      }
                                      return make_upload_validation_response(validation);
                                    });
  };
}

} // namespace hps
