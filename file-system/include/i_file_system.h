#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace hps {

/**
 * 文件分块描述（存引用不存数据）
 *
 * split_file 仅计算分块边界，不读取文件内容；
 * compute_chunk_hash 时按 offset seek + 按需读取。
 */
struct FileChunk {
  std::string source_path;  ///< 源文件路径
  std::size_t offset;       ///< 块在文件中的起始偏移（字节）
  std::size_t size;         ///< 块大小（字节）
};

/**
 * 文件系统接口（抽象类）
 *
 * 文件切割/哈希/存储管理。非热点路径，使用动态多态。
 * path 参数为相对 base_dir 的虚拟路径，由实现负责路径解析与安全校验。
 */
class IFileSystem {
public:
  virtual ~IFileSystem() = default;

  /** 文件切割：按 chunk_size 将文件切分为多个 FileChunk（不读取内容） */
  virtual std::vector<FileChunk> split_file(const std::string& path, std::size_t chunk_size) = 0;

  /** 完整哈希：对整个文件计算 SHA-256，返回 64 字符十六进制字符串 */
  virtual std::string compute_file_hash(const std::string& path) = 0;

  /** 分块哈希：对单个 FileChunk 计算 SHA-256，返回 64 字符十六进制字符串 */
  virtual std::string compute_chunk_hash(const FileChunk& chunk) = 0;

  /** 存储文件：将 data 写入虚拟路径（自动创建父目录） */
  virtual bool store_file(const std::string& path, const std::vector<char>& data) = 0;

  /** 删除文件：删除虚拟路径对应文件 */
  virtual bool delete_file(const std::string& path) = 0;

  /** 读取文件：返回虚拟路径对应文件内容，不存在返回 nullopt */
  virtual std::optional<std::vector<char>> read_file(const std::string& path) = 0;
};

}  // namespace hps
