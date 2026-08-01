#pragma once

#include "i_file_system.h"

#include <string>

namespace hps {

/**
 * 文件系统实现
 *
 * 基于 base_dir 的虚拟路径文件管理，支持文件切割、SHA-256 哈希、存储管理。
 * 虚拟路径经 resolve_path 映射到 base_dir 下的实际路径，防路径穿越。
 */
class FileSystem : public IFileSystem {
public:
  /**
   * 构造文件系统
   * @param base_dir 存储根目录（虚拟路径映射基准）
   */
  explicit FileSystem(std::string base_dir);

  std::vector<FileChunk> split_file(const std::string& path, std::size_t chunk_size) override;
  std::string compute_file_hash(const std::string& path) override;
  std::string compute_chunk_hash(const FileChunk& chunk) override;
  bool store_file(const std::string& path, const std::vector<char>& data) override;
  bool delete_file(const std::string& path) override;
  ChunkDeleteStatus delete_file_status(const std::string& path) override;
  std::optional<std::vector<char>> read_file(const std::string& path) override;

  /** 对指定数据计算 SHA-256 并返回十六进制字符串 */
  static std::string sha256_hex(const char* data, std::size_t len);

private:
  std::string base_dir_;

  /**
   * 虚拟路径解析为实际路径
   * @return 实际路径；若检测到路径穿越（.. 逃出 base_dir）返回空字符串
   */
  std::string resolve_path(const std::string& virtual_path) const;

  /** 二进制哈希转十六进制字符串 */
  static std::string to_hex(const unsigned char* hash, unsigned int len);
};

} // namespace hps
