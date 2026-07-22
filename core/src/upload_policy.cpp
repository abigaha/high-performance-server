#include "upload_policy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace hps {

namespace {

struct AudioType {
  std::string_view extension;
  std::string_view content_type;
};

constexpr std::array<AudioType, 9> kAudioTypes{{
  {".mp3", "audio/mpeg"},
  {".ogg", "audio/ogg"},
  {".wav", "audio/wav"},
  {".flac", "audio/flac"},
  {".aac", "audio/aac"},
  {".m4a", "audio/mp4"},
  {".wma", "audio/x-ms-wma"},
  {".ape", "audio/x-monkeys-audio"},
  {".opus", "audio/opus"},
}};

std::string lowercase_extension(std::string_view file_name) {
  const auto dot = file_name.rfind('.');
  if (dot == std::string_view::npos || dot + 1 >= file_name.size()) {
    return {};
  }
  std::string extension(file_name.substr(dot));
  std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return extension;
}

std::size_t role_size_limit(UserRole role, const ServerConfig& config) {
  if (role == UserRole::NORMAL && config.normal_max_size > 0) {
    return static_cast<std::size_t>(config.normal_max_size);
  }
  if (role == UserRole::VIP && config.vip_max_size > 0) {
    return static_cast<std::size_t>(config.vip_max_size);
  }
  return 0;
}

std::string status_text(int status_code) {
  switch (status_code) {
    case 413:
      return "Payload Too Large";
    case 415:
      return "Unsupported Media Type";
    default:
      return "Bad Request";
  }
}

UploadValidationResult reject_upload(int status_code, std::string code, std::string error) {
  UploadValidationResult result;
  result.status_code = status_code;
  result.code = std::move(code);
  result.error = std::move(error);
  return result;
}

} // namespace

std::optional<std::string_view> audio_content_type(std::string_view file_name) {
  const auto extension = lowercase_extension(file_name);
  const auto* const it =
    std::ranges::find_if(kAudioTypes, [&extension](const AudioType& type) { return type.extension == extension; });
  if (it == kAudioTypes.end()) {
    return std::nullopt;
  }
  return it->content_type;
}

UploadValidationResult validate_audio_upload(std::string_view file_name,
                                             std::optional<std::size_t> content_length,
                                             UserRole role,
                                             const ServerConfig& config) {
  if (file_name.empty()) {
    return reject_upload(400, "INVALID_FILE_NAME", "缺少有效文件名");
  }

  const auto content_type = audio_content_type(file_name);
  if (!content_type) {
    return reject_upload(415,
                         "UNSUPPORTED_FILE_TYPE",
                         "不支持的文件类型，仅允许 MP3、OGG、WAV、FLAC、AAC、M4A、WMA、APE、OPUS");
  }

  if (!content_length) {
    return reject_upload(400, "INVALID_CONTENT_LENGTH", "缺少有效的 Content-Length，无法安全上传");
  }

  if (*content_length == 0) {
    return reject_upload(400, "EMPTY_FILE", "文件内容为空，不能上传零字节文件");
  }

  const auto max_size = role_size_limit(role, config);
  if (max_size > 0 && *content_length > max_size) {
    auto result = reject_upload(413, "FILE_TOO_LARGE", "文件大小超过当前账户限制");
    result.file_size = *content_length;
    result.max_size = max_size;
    return result;
  }

  UploadValidationResult result;
  result.accepted = true;
  result.status_code = 200;
  result.code = "OK";
  result.content_type = *content_type;
  result.file_size = *content_length;
  result.max_size = max_size;
  return result;
}

HttpResponse make_upload_validation_response(const UploadValidationResult& validation) {
  HttpResponse response;
  response.set_status(validation.status_code, status_text(validation.status_code));
  response.set_content_type("application/json; charset=utf-8");

  nlohmann::json body{{"error", validation.error}, {"code", validation.code}};
  if (validation.status_code == 415) {
    body["details"] = {{"allowed_extensions", {"mp3", "ogg", "wav", "flac", "aac", "m4a", "wma", "ape", "opus"}}};
  } else if (validation.status_code == 413) {
    body["details"] = {{"file_size", validation.file_size}, {"max_size", validation.max_size}};
  }
  response.body = body.dump();
  response.set_content_length(response.body.size());
  return response;
}

} // namespace hps
