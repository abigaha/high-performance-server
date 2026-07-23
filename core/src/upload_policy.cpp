#include "upload_policy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
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

constexpr std::array<unsigned char, 16> kAsfHeaderGuid{{
  0x30U,
  0x26U,
  0xB2U,
  0x75U,
  0x8EU,
  0x66U,
  0xCFU,
  0x11U,
  0xA6U,
  0xD9U,
  0x00U,
  0xAAU,
  0x00U,
  0x62U,
  0xCEU,
  0x6CU,
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

bool starts_with(std::string_view value, std::string_view expected) {
  return value.size() >= expected.size() && value.substr(0, expected.size()) == expected;
}

bool has_bytes_at(std::string_view value, std::size_t offset, std::size_t count) {
  return offset <= value.size() && count <= value.size() - offset;
}

std::uint32_t byte_at(std::string_view value, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset]));
}

std::uint16_t read_le16(std::string_view value, std::size_t offset) {
  return static_cast<std::uint16_t>(byte_at(value, offset) | (byte_at(value, offset + 1U) << 8U));
}

std::uint32_t read_le32(std::string_view value, std::size_t offset) {
  return byte_at(value, offset) | (byte_at(value, offset + 1U) << 8U) | (byte_at(value, offset + 2U) << 16U) |
         (byte_at(value, offset + 3U) << 24U);
}

std::uint32_t read_be32(std::string_view value, std::size_t offset) {
  return (byte_at(value, offset) << 24U) | (byte_at(value, offset + 1U) << 16U) | (byte_at(value, offset + 2U) << 8U) |
         byte_at(value, offset + 3U);
}

bool is_mpeg_frame_header_at(std::string_view prefix, std::size_t offset) {
  if (!has_bytes_at(prefix, offset, 4)) {
    return false;
  }
  const auto first = static_cast<unsigned char>(prefix[offset]);
  const auto second = static_cast<unsigned char>(prefix[offset + 1]);
  const auto third = static_cast<unsigned char>(prefix[offset + 2]);
  return first == 0xFFU && (second & 0xE0U) == 0xE0U && (second & 0x18U) != 0x08U && (second & 0x06U) != 0U &&
         (third & 0xF0U) != 0xF0U && (third & 0x0CU) != 0x0CU;
}

bool is_id3v2_header_followed_by_mpeg_frame(std::string_view prefix) {
  constexpr std::size_t kId3HeaderSize = 10;
  if (!has_bytes_at(prefix, 0, kId3HeaderSize) || !starts_with(prefix, "ID3") || prefix[3] < 2 || prefix[3] > 4) {
    return false;
  }

  for (std::size_t index = 6; index < kId3HeaderSize; ++index) {
    if ((static_cast<unsigned char>(prefix[index]) & 0x80U) != 0U) {
      return false;
    }
  }

  const auto tag_size = (static_cast<std::size_t>(static_cast<unsigned char>(prefix[6])) << 21U) |
                        (static_cast<std::size_t>(static_cast<unsigned char>(prefix[7])) << 14U) |
                        (static_cast<std::size_t>(static_cast<unsigned char>(prefix[8])) << 7U) |
                        static_cast<std::size_t>(static_cast<unsigned char>(prefix[9]));
  return is_mpeg_frame_header_at(prefix, kId3HeaderSize + tag_size);
}

bool is_adts_header(std::string_view prefix) {
  constexpr std::size_t kAdtsHeaderWithoutCrcSize = 7;
  if (prefix.size() < kAdtsHeaderWithoutCrcSize) {
    return false;
  }
  const auto first = byte_at(prefix, 0);
  const auto second = byte_at(prefix, 1);
  const auto third = byte_at(prefix, 2);
  const auto fourth = byte_at(prefix, 3);
  const auto fifth = byte_at(prefix, 4);
  const auto sixth = byte_at(prefix, 5);
  const auto header_size = (second & 0x01U) == 0U ? 9U : kAdtsHeaderWithoutCrcSize;
  if (prefix.size() < header_size) {
    return false;
  }
  const auto frame_size = (static_cast<std::size_t>(fourth & 0x03U) << 11U) | (static_cast<std::size_t>(fifth) << 3U) |
                          static_cast<std::size_t>((sixth >> 5U) & 0x07U);
  const auto channel_configuration = ((third & 0x01U) << 2U) | ((fourth >> 6U) & 0x03U);
  const auto sampling_frequency_index = (third >> 2U) & 0x0FU;
  return first == 0xFFU && (second & 0xF6U) == 0xF0U && sampling_frequency_index < 13U && channel_configuration > 0U &&
         frame_size >= header_size;
}

bool is_asf_header(std::string_view prefix) {
  constexpr std::size_t kAsfHeaderObjectSize = 30;
  if (prefix.size() < kAsfHeaderObjectSize) {
    return false;
  }
  for (std::size_t index = 0; index < kAsfHeaderGuid.size(); ++index) {
    if (static_cast<unsigned char>(prefix[index]) != kAsfHeaderGuid[index]) {
      return false;
    }
  }
  const auto object_size = static_cast<uint64_t>(read_le32(prefix, 16)) |
                           (static_cast<uint64_t>(read_le32(prefix, 20)) << 32U);
  const auto object_count = read_le32(prefix, 24);
  return object_size >= kAsfHeaderObjectSize && object_count > 0U && prefix[28] == '\x01' && prefix[29] == '\x02';
}

std::optional<std::string_view> first_ogg_packet(std::string_view prefix) {
  constexpr std::size_t kOggFixedHeaderSize = 27;
  if (prefix.size() < kOggFixedHeaderSize || !starts_with(prefix, "OggS") || prefix[4] != '\0') {
    return std::nullopt;
  }
  const auto segment_count = static_cast<unsigned char>(prefix[26]);
  if (segment_count == 0U || !has_bytes_at(prefix, kOggFixedHeaderSize, segment_count)) {
    return std::nullopt;
  }

  std::size_t packet_size = 0;
  bool packet_ends = false;
  for (std::size_t index = 0; index < segment_count; ++index) {
    const auto lacing_value = static_cast<unsigned char>(prefix[kOggFixedHeaderSize + index]);
    packet_size += lacing_value;
    if (lacing_value < 255U) {
      packet_ends = true;
      break;
    }
  }
  if (!packet_ends || packet_size == 0U) {
    return std::nullopt;
  }

  const auto payload_offset = kOggFixedHeaderSize + static_cast<std::size_t>(segment_count);
  if (!has_bytes_at(prefix, payload_offset, packet_size)) {
    return std::nullopt;
  }
  return prefix.substr(payload_offset, packet_size);
}

bool is_vorbis_ogg_stream(std::string_view prefix) {
  constexpr std::size_t kVorbisIdentificationHeaderSize = 30;
  const auto packet = first_ogg_packet(prefix);
  if (!packet || packet->size() < kVorbisIdentificationHeaderSize || !starts_with(*packet, "\x01vorbis")) {
    return false;
  }
  const auto version = read_le32(*packet, 7);
  const auto channels = byte_at(*packet, 11);
  const auto sample_rate = read_le32(*packet, 12);
  const auto block_sizes = byte_at(*packet, 28);
  const auto lower_block_size = block_sizes & 0x0FU;
  const auto upper_block_size = block_sizes >> 4U;
  const auto framing_flag = byte_at(*packet, 29);
  return version == 0U && channels > 0U && sample_rate > 0U && lower_block_size > 0U &&
         lower_block_size <= upper_block_size && (framing_flag & 0x01U) != 0U;
}

bool is_opus_ogg_stream(std::string_view prefix) {
  constexpr std::size_t kOpusHeadSize = 19;
  const auto packet = first_ogg_packet(prefix);
  if (!packet || packet->size() < kOpusHeadSize || !starts_with(*packet, "OpusHead")) {
    return false;
  }
  const auto version = byte_at(*packet, 8);
  const auto channels = byte_at(*packet, 9);
  const auto channel_mapping_family = byte_at(*packet, 18);
  return version == 1U && channels > 0U && (channel_mapping_family == 0U || packet->size() >= 21U + channels);
}

struct RiffChunk {
  std::string_view id;
  std::size_t payload_offset;
  std::size_t payload_size;
  std::size_t payload_end;
  std::size_t next_offset;
  bool has_padding;
};

std::optional<std::size_t> wav_riff_end(std::string_view prefix) {
  constexpr std::size_t kRiffHeaderSize = 12;
  if (!has_bytes_at(prefix, 0, kRiffHeaderSize) || !starts_with(prefix, "RIFF") || prefix.substr(8, 4) != "WAVE") {
    return std::nullopt;
  }

  const auto riff_size = static_cast<std::size_t>(read_le32(prefix, 4));
  if (riff_size < 4U || riff_size > std::numeric_limits<std::size_t>::max() - 8U) {
    return std::nullopt;
  }
  return 8U + riff_size;
}

std::optional<RiffChunk> parse_riff_chunk(std::string_view prefix, std::size_t chunk_offset, std::size_t riff_end) {
  constexpr std::size_t kRiffChunkHeaderSize = 8;
  if (chunk_offset >= riff_end || riff_end - chunk_offset < kRiffChunkHeaderSize ||
      !has_bytes_at(prefix, chunk_offset, kRiffChunkHeaderSize)) {
    return std::nullopt;
  }

  const auto payload_size = static_cast<std::size_t>(read_le32(prefix, chunk_offset + 4U));
  const auto payload_offset = chunk_offset + kRiffChunkHeaderSize;
  if (payload_size > riff_end - payload_offset) {
    return std::nullopt;
  }
  const auto payload_end = payload_offset + payload_size;
  const auto has_padding = (payload_size & 1U) != 0U;
  if (has_padding && payload_end == riff_end) {
    return std::nullopt;
  }

  return RiffChunk{prefix.substr(chunk_offset, 4),
                   payload_offset,
                   payload_size,
                   payload_end,
                   payload_end + (has_padding ? 1U : 0U),
                   has_padding};
}

bool has_complete_riff_chunk(std::string_view prefix, const RiffChunk& chunk) {
  return has_bytes_at(prefix, chunk.payload_offset, chunk.payload_size) &&
         (!chunk.has_padding || has_bytes_at(prefix, chunk.payload_end, 1U));
}

bool is_valid_wav_format_chunk(std::string_view prefix, const RiffChunk& chunk) {
  constexpr std::size_t kWavRequiredFormatSize = 16;
  if (chunk.payload_size < kWavRequiredFormatSize || !has_complete_riff_chunk(prefix, chunk)) {
    return false;
  }

  const auto audio_format = read_le16(prefix, chunk.payload_offset);
  const auto channel_count = read_le16(prefix, chunk.payload_offset + 2U);
  const auto sample_rate = read_le32(prefix, chunk.payload_offset + 4U);
  const auto byte_rate = read_le32(prefix, chunk.payload_offset + 8U);
  const auto block_alignment = read_le16(prefix, chunk.payload_offset + 12U);
  const auto bits_per_sample = read_le16(prefix, chunk.payload_offset + 14U);
  return (audio_format == 1U || audio_format == 3U || audio_format == 0xFFFEU) && channel_count > 0U &&
         sample_rate > 0U && byte_rate > 0U && block_alignment > 0U && bits_per_sample > 0U;
}

bool is_valid_wav_data_chunk(std::string_view prefix, const RiffChunk& chunk, bool has_valid_format) {
  if (!has_valid_format || chunk.payload_size == 0U || !has_bytes_at(prefix, chunk.payload_offset, 1U)) {
    return false;
  }
  return prefix.size() >= kAudioSignatureProbeSize || has_complete_riff_chunk(prefix, chunk);
}

bool is_wav_header(std::string_view prefix) {
  const auto riff_end = wav_riff_end(prefix);
  if (!riff_end) {
    return false;
  }

  bool has_valid_format = false;
  for (std::size_t chunk_offset = 12U; chunk_offset < *riff_end;) {
    const auto chunk = parse_riff_chunk(prefix, chunk_offset, *riff_end);
    if (!chunk) {
      return false;
    }

    if (chunk->id == "fmt ") {
      if (has_valid_format || !is_valid_wav_format_chunk(prefix, *chunk)) {
        return false;
      }
      has_valid_format = true;
    } else if (chunk->id == "data") {
      return is_valid_wav_data_chunk(prefix, *chunk, has_valid_format);
    } else if (!has_complete_riff_chunk(prefix, *chunk)) {
      return false;
    }
    chunk_offset = chunk->next_offset;
  }
  return false;
}

bool is_flac_header(std::string_view prefix) {
  constexpr std::size_t kFlacStreamInfoSize = 34;
  constexpr std::size_t kFlacHeaderSize = 4;
  constexpr std::size_t kFlacMetadataHeaderSize = 4;
  constexpr std::size_t kFlacMinimumHeaderSize = kFlacHeaderSize + kFlacMetadataHeaderSize + kFlacStreamInfoSize;
  if (!has_bytes_at(prefix, 0, kFlacMinimumHeaderSize) || !starts_with(prefix, "fLaC") ||
      (byte_at(prefix, 4) & 0x7FU) != 0U ||
      (byte_at(prefix, 5) != 0U || byte_at(prefix, 6) != 0U || byte_at(prefix, 7) != kFlacStreamInfoSize)) {
    return false;
  }

  const auto minimum_block_size = read_le16(prefix, 8);
  const auto maximum_block_size = read_le16(prefix, 10);
  const auto sample_rate = (byte_at(prefix, 18) << 12U) | (byte_at(prefix, 19) << 4U) | (byte_at(prefix, 20) >> 4U);
  return minimum_block_size > 0U && maximum_block_size >= minimum_block_size && sample_rate > 0U;
}

bool is_m4a_header(std::string_view prefix) {
  constexpr std::size_t kMinimumFtypBoxSize = 20;
  if (!has_bytes_at(prefix, 0, kMinimumFtypBoxSize) || prefix.substr(4, 4) != "ftyp") {
    return false;
  }
  const auto box_size = static_cast<std::size_t>(read_be32(prefix, 0));
  if (box_size < kMinimumFtypBoxSize || !has_bytes_at(prefix, 0, box_size)) {
    return false;
  }
  for (std::size_t offset = 8; offset + 4 <= box_size; offset += 4) {
    const auto brand = prefix.substr(offset, 4);
    if (brand == "M4A " || brand == "M4B " || brand == "M4P ") {
      return true;
    }
  }
  return false;
}

bool is_ape_header(std::string_view prefix) {
  constexpr std::size_t kApeDescriptorSize = 52;
  if (!has_bytes_at(prefix, 0, kApeDescriptorSize) || !starts_with(prefix, "MAC ")) {
    return false;
  }
  const auto version = read_le16(prefix, 4);
  const auto descriptor_size = read_le32(prefix, 8);
  const auto header_size = read_le32(prefix, 12);
  const auto frame_data_size = read_le32(prefix, 24);
  return version >= 3800U && version <= 5000U && descriptor_size >= kApeDescriptorSize && header_size > 0U &&
         frame_data_size > 0U;
}

bool matches_audio_signature(std::string_view extension, const AudioSignaturePrefix& signature_prefix) {
  const auto prefix = signature_prefix.value();
  if (extension == ".mp3") {
    return is_id3v2_header_followed_by_mpeg_frame(prefix) || is_mpeg_frame_header_at(prefix, 0);
  }
  if (extension == ".ogg") {
    return is_vorbis_ogg_stream(prefix) || is_opus_ogg_stream(prefix);
  }
  if (extension == ".wav") {
    return is_wav_header(prefix);
  }
  if (extension == ".flac") {
    return is_flac_header(prefix);
  }
  if (extension == ".aac") {
    return is_adts_header(prefix);
  }
  if (extension == ".m4a") {
    return is_m4a_header(prefix);
  }
  if (extension == ".wma") {
    return is_asf_header(prefix);
  }
  if (extension == ".ape") {
    return is_ape_header(prefix);
  }
  if (extension == ".opus") {
    return is_opus_ogg_stream(prefix);
  }
  return false;
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

UploadValidationResult validate_audio_signature(std::string_view file_name,
                                                const AudioSignaturePrefix& signature_prefix) {
  const auto extension = lowercase_extension(file_name);
  const auto content_type = audio_content_type(file_name);
  if (!content_type) {
    return reject_upload(415,
                         "UNSUPPORTED_FILE_TYPE",
                         "不支持的文件类型，仅允许 MP3、OGG、WAV、FLAC、AAC、M4A、WMA、APE、OPUS");
  }
  if (!matches_audio_signature(extension, signature_prefix)) {
    return reject_upload(415, "INVALID_AUDIO_SIGNATURE", "文件内容签名与文件扩展名不匹配或文件头不完整");
  }

  UploadValidationResult result;
  result.accepted = true;
  result.status_code = 200;
  result.code = "OK";
  result.content_type = *content_type;
  return result;
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
