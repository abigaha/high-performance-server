# Step 4：file-system 文件读取 + 虚拟路径解析

> **状态**：✅ 已完成（commit `38953e5`）
> **起止**：接口层落地后

## 背景

音乐软件需要管理文件存储，包括文件切割、哈希计算、存储管理以及防止路径穿越的安全防护。

## 功能点

| # | 功能点 | 说明 |
|---|--------|------|
| F1 | **IFileSystem 抽象接口** | 文件系统操作抽象层 |
| F2 | **FileSystem 实现** | 文件切割、哈希计算（SHA-256）|
| F3 | **存储管理** | store_file / read_file / delete_file + 自动建目录 |
| F4 | **虚拟路径解析** | resolve_path + 路径穿越防护 |

## 接口设计

```cpp
struct FileChunk {
  std::size_t index;
  std::size_t offset;
  std::size_t size;
  std::string hash;
};

class IFileSystem {
public:
  virtual ~IFileSystem() = default;
  virtual std::vector<FileChunk> split_file(const std::string& path, std::size_t chunk_size) = 0;
  virtual std::string compute_file_hash(const std::string& path) = 0;
  virtual std::string compute_chunk_hash(const FileChunk& chunk) = 0;
  virtual bool store_file(const std::string& path, const std::vector<char>& data) = 0;
  virtual bool delete_file(const std::string& path) = 0;
  virtual std::optional<std::vector<char>> read_file(const std::string& path) = 0;
};
```

## 文件清单

| 路径 | 说明 |
|------|------|
| `file-system/include/i_file_system.h` | 抽象接口 |
| `file-system/include/file_system.h` | 具体实现声明 |
| `file-system/src/file_system.cpp` | 实现（split/hash/store/delete/resolve）|
| `file-system/xmake.lua` | 构建配置 |

## 测试用例

| 测试文件 | 用例数 | 覆盖场景 |
|---------|--------|---------|
| `test_file_system.cpp` | 14 | split/hash/store/delete/路径穿越 |

## 质量门禁

| 检查项 | 结果 |
|--------|------|
| clang-tidy | ✅ 0 / 0 / 0 |
| cppcheck | ✅ 0 / 0 / 0 / 0 |
| 编译 | ✅ 0 error / 0 warning |
| 测试 | ✅ 14/14 通过 |
