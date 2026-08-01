#include "file_system.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace hps {

namespace {

constexpr std::size_t kHashReadBufferSize = 65536; // 64KB 流式读取缓冲

} // namespace

FileSystem::FileSystem(std::string base_dir) : base_dir_(std::move(base_dir)) {
  if (!base_dir_.empty() && base_dir_.back() == '/') {
    base_dir_.pop_back();
  }
}

std::string FileSystem::to_hex(const unsigned char* hash, unsigned int len) {
  static constexpr std::array<char, 17> kHexChars = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f', '\0'};
  std::string hex(static_cast<std::size_t>(len) * 2, '\0');
  for (unsigned int i = 0; i < len; ++i) {
    std::size_t idx = static_cast<std::size_t>(i) * 2;
    unsigned int high = static_cast<unsigned int>(hash[i]) >> 4U;
    unsigned int low = static_cast<unsigned int>(hash[i]) & 0x0FU;
    hex[idx] = kHexChars[high];
    hex[idx + 1] = kHexChars[low];
  }
  return hex;
}

std::string FileSystem::sha256_hex(const char* data, std::size_t len) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
  unsigned int hash_len = 0;
  auto* ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  EVP_DigestUpdate(ctx, data, len);
  EVP_DigestFinal_ex(ctx, hash.data(), &hash_len);
  EVP_MD_CTX_free(ctx);
  return to_hex(hash.data(), hash_len);
}

std::string FileSystem::resolve_path(const std::string& virtual_path) const {
  if (virtual_path.empty()) {
    return {};
  }

  // 拼接实际路径
  fs::path full = fs::path(base_dir_) / virtual_path;

  // weakly_canonical 解析 .. 并规范化（不要求文件存在）
  std::error_code ec;
  fs::path resolved = fs::weakly_canonical(full, ec);
  if (ec) {
    return {};
  }

  // 规范化 base_dir 用于前缀比较
  fs::path base_resolved = fs::weakly_canonical(base_dir_, ec);
  if (ec) {
    base_resolved = base_dir_;
  }

  // 路径穿越防护：resolved 必须在 base_resolved 之下
  auto resolved_str = resolved.string();
  auto base_str = base_resolved.string();
  if (resolved_str == base_str) {
    return {}; // 虚拟路径解析到 base_dir 本身，不允许
  }
  // 检查 resolved 是否以 base_dir/ 开头
  if (resolved_str.size() <= base_str.size() || resolved_str.compare(0, base_str.size(), base_str) != 0 ||
      resolved_str[base_str.size()] != '/') {
    return {};
  }

  return resolved_str;
}

std::vector<FileChunk> FileSystem::split_file(const std::string& path, std::size_t chunk_size) {
  std::vector<FileChunk> chunks;
  if (chunk_size == 0) {
    return chunks;
  }

  std::error_code ec;
  if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) {
    return chunks;
  }

  auto file_size = fs::file_size(path, ec);
  if (ec) {
    return chunks;
  }

  std::size_t offset = 0;
  while (offset < file_size) {
    std::size_t remaining = file_size - offset;
    std::size_t this_size = std::min(chunk_size, remaining);
    chunks.push_back(FileChunk{path, offset, this_size});
    offset += this_size;
  }

  return chunks;
}

std::string FileSystem::compute_file_hash(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }

  auto* ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);

  std::vector<char> buffer(kHashReadBufferSize);
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    auto bytes_read = static_cast<std::size_t>(file.gcount());
    if (bytes_read > 0) {
      EVP_DigestUpdate(ctx, buffer.data(), bytes_read);
    }
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
  unsigned int hash_len = 0;
  EVP_DigestFinal_ex(ctx, hash.data(), &hash_len);
  EVP_MD_CTX_free(ctx);
  auto hex = to_hex(hash.data(), hash_len);
  return hex;
}

std::string FileSystem::compute_chunk_hash(const FileChunk& chunk) {
  std::ifstream file(chunk.source_path, std::ios::binary);
  if (!file) {
    return {};
  }

  file.seekg(static_cast<std::streamoff>(chunk.offset), std::ios::beg);
  if (!file) {
    return {};
  }

  auto* ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);

  std::size_t remaining = chunk.size;
  std::vector<char> buffer(kHashReadBufferSize);
  while (remaining > 0) {
    std::size_t to_read = std::min(remaining, buffer.size());
    file.read(buffer.data(), static_cast<std::streamsize>(to_read));
    auto bytes_read = static_cast<std::size_t>(file.gcount());
    if (bytes_read == 0) {
      break;
    }
    EVP_DigestUpdate(ctx, buffer.data(), bytes_read);
    remaining -= bytes_read;
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
  unsigned int hash_len = 0;
  EVP_DigestFinal_ex(ctx, hash.data(), &hash_len);
  EVP_MD_CTX_free(ctx);
  return to_hex(hash.data(), hash_len);
}

bool FileSystem::store_file(const std::string& path, const std::vector<char>& data) {
  std::string resolved = resolve_path(path);
  if (resolved.empty()) {
    return false;
  }

  fs::path file_path(resolved);
  std::error_code ec;
  fs::create_directories(file_path.parent_path(), ec);
  if (ec) {
    return false;
  }

  std::ofstream file(file_path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }

  file.write(data.data(), static_cast<std::streamsize>(data.size()));
  return file.good();
}

bool FileSystem::delete_file(const std::string& path) {
  return delete_file_status(path) == ChunkDeleteStatus::DELETED;
}

ChunkDeleteStatus FileSystem::delete_file_status(const std::string& path) {
  std::string resolved = resolve_path(path);
  if (resolved.empty()) {
    return ChunkDeleteStatus::ERROR;
  }

  std::error_code ec;
  const bool removed = fs::remove(resolved, ec);
  if (ec) {
    return ChunkDeleteStatus::ERROR;
  }
  return removed ? ChunkDeleteStatus::DELETED : ChunkDeleteStatus::NOT_FOUND;
}

std::optional<std::vector<char>> FileSystem::read_file(const std::string& path) {
  std::string resolved = resolve_path(path);
  if (resolved.empty()) {
    return std::nullopt;
  }

  std::ifstream file(resolved, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }

  std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return data;
}

} // namespace hps
