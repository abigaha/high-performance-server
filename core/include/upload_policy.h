#pragma once

#include "http_response.h"
#include "main_functions.h"
#include "models.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace hps {

inline constexpr std::size_t kAudioSignatureProbeSize = 512;

class AudioSignaturePrefix {
public:
  explicit constexpr AudioSignaturePrefix(std::string_view value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::string_view value() const noexcept { return value_; }

private:
  std::string_view value_;
};

struct UploadValidationResult {
  bool accepted{false};
  int status_code{400};
  std::string code;
  std::string error;
  std::string content_type;
  std::size_t file_size{0};
  std::size_t max_size{0};
};

std::optional<std::string_view> audio_content_type(std::string_view file_name);

UploadValidationResult validate_audio_signature(std::string_view file_name, const AudioSignaturePrefix& prefix);

UploadValidationResult validate_audio_upload(std::string_view file_name,
                                             std::optional<std::size_t> content_length,
                                             UserRole role,
                                             const ServerConfig& config);

HttpResponse make_upload_validation_response(const UploadValidationResult& validation);

} // namespace hps
