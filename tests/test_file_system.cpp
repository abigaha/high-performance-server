#include "file_system.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// 测试用临时目录 RAII 封装
class TempDir {
public:
  TempDir() {
    auto* tmpdir = std::getenv("TMPDIR");
    std::string base = (tmpdir != nullptr) ? tmpdir : "/tmp";
    path_ = base + "/hps_test_fs_" + std::to_string(getpid());
    fs::create_directories(path_);
  }

  ~TempDir() { fs::remove_all(path_); }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

  std::string file_path(const std::string& name) const { return path_ + "/" + name; }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) 测试辅助函数，两 string 参数语义清晰
  void write_file(const std::string& name, const std::string& content) const {
    std::ofstream f(file_path(name), std::ios::binary);
    f << content;
  }

private:
  std::string path_;
};

// 已知 SHA-256 测试向量（空字符串）
constexpr const char* kEmptySha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

// 已知 SHA-256 测试向量（"hello"）
constexpr const char* kHelloSha256 = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";

} // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// T1: 文件大小整除 chunk_size
// NOLINTNEXTLINE(readability-function-cognitive-complexity) gtest 宏展开导致嵌套层级虚高
TEST(FileSystemTest, SplitFileEvenChunks) {
  TempDir tmp;
  std::string content(100, 'x'); // 100 字节
  tmp.write_file("test.bin", content);

  hps::FileSystem fs(tmp.path());
  auto chunks = fs.split_file(tmp.file_path("test.bin"), 25);

  ASSERT_EQ(chunks.size(), 4U);
  EXPECT_EQ(chunks[0].offset, 0U);
  EXPECT_EQ(chunks[0].size, 25U);
  EXPECT_EQ(chunks[1].offset, 25U);
  EXPECT_EQ(chunks[1].size, 25U);
  EXPECT_EQ(chunks[3].offset, 75U);
  EXPECT_EQ(chunks[3].size, 25U);
}

// T2: 非整除，最后一块 size < chunk_size
TEST(FileSystemTest, SplitFileUnevenLastChunk) {
  TempDir tmp;
  std::string content(100, 'y');
  tmp.write_file("test.bin", content);

  hps::FileSystem fs(tmp.path());
  auto chunks = fs.split_file(tmp.file_path("test.bin"), 30);

  ASSERT_EQ(chunks.size(), 4U);
  EXPECT_EQ(chunks[3].offset, 90U);
  EXPECT_EQ(chunks[3].size, 10U); // 最后一块 10 字节
}

// T3: chunk_size=0 返回空 vector
TEST(FileSystemTest, SplitFileChunkSizeZero) {
  TempDir tmp;
  tmp.write_file("test.bin", "data");

  hps::FileSystem fs(tmp.path());
  auto chunks = fs.split_file(tmp.file_path("test.bin"), 0);
  EXPECT_TRUE(chunks.empty());
}

// T4: 文件不存在返回空 vector
TEST(FileSystemTest, SplitFileNotExist) {
  TempDir tmp;
  hps::FileSystem fs(tmp.path());
  auto chunks = fs.split_file(tmp.file_path("nonexistent.bin"), 1024);
  EXPECT_TRUE(chunks.empty());
}

// T5: 完整哈希验证
TEST(FileSystemTest, ComputeFileHash) {
  TempDir tmp;
  tmp.write_file("hello.txt", "hello");

  hps::FileSystem fs(tmp.path());
  std::string hash = fs.compute_file_hash(tmp.file_path("hello.txt"));
  EXPECT_EQ(hash, kHelloSha256);
}

// T6: 文件不存在哈希返回空
TEST(FileSystemTest, ComputeFileHashNotExist) {
  TempDir tmp;
  hps::FileSystem fs(tmp.path());
  std::string hash = fs.compute_file_hash(tmp.file_path("nonexistent.txt"));
  EXPECT_TRUE(hash.empty());
}

// T7: 分块哈希验证
TEST(FileSystemTest, ComputeChunkHash) {
  TempDir tmp;
  std::string content = "hello";
  tmp.write_file("hello.txt", content);

  hps::FileSystem fs(tmp.path());
  hps::FileChunk chunk{tmp.file_path("hello.txt"), 0, content.size()};
  std::string hash = fs.compute_chunk_hash(chunk);
  EXPECT_EQ(hash, kHelloSha256);
}

// T8: store + read 数据一致
TEST(FileSystemTest, StoreAndReadFile) {
  TempDir tmp;
  hps::FileSystem fs(tmp.path());

  std::vector<char> data{'a', 'b', 'c', 'd', 'e'};
  ASSERT_TRUE(fs.store_file("sub/dir/test.dat", data));

  auto read_back = fs.read_file("sub/dir/test.dat");
  ASSERT_TRUE(read_back.has_value());
  EXPECT_EQ(read_back->size(), data.size());
  EXPECT_EQ(*read_back, data);
}

// T9: 读取不存在的文件返回 nullopt
TEST(FileSystemTest, ReadFileNotExist) {
  TempDir tmp;
  hps::FileSystem fs(tmp.path());
  auto result = fs.read_file("nonexistent.dat");
  EXPECT_FALSE(result.has_value());
}

// T10: 删除文件后读取返回 nullopt
TEST(FileSystemTest, DeleteFile) {
  TempDir tmp;
  hps::FileSystem fs(tmp.path());

  std::vector<char> data{'x', 'y', 'z'};
  ASSERT_TRUE(fs.store_file("to_delete.dat", data));
  ASSERT_TRUE(fs.delete_file("to_delete.dat"));

  auto result = fs.read_file("to_delete.dat");
  EXPECT_FALSE(result.has_value());
}

// T11: 删除不存在的文件返回 false
TEST(FileSystemTest, DeleteFileNotExist) {
  TempDir tmp;
  hps::FileSystem fs(tmp.path());
  EXPECT_FALSE(fs.delete_file("nonexistent.dat"));
}

// T12: 路径穿越防护
TEST(FileSystemTest, PathTraversalRejected) {
  TempDir tmp;
  hps::FileSystem fs(tmp.path());

  std::vector<char> data{'s', 'e', 'c', 'r', 'e', 't'};

  // .. 尝试逃出 base_dir —— 所有操作均应失败
  std::string evil_path = "../../etc/passwd";
  EXPECT_FALSE(fs.store_file(evil_path, data));
  EXPECT_FALSE(fs.read_file(evil_path).has_value());
  EXPECT_FALSE(fs.delete_file(evil_path));
}

// T12b: 正常路径在穿越防护下仍可用
TEST(FileSystemTest, NormalPathStillWorks) {
  TempDir tmp;
  hps::FileSystem fs(tmp.path());

  std::vector<char> data{'o', 'k'};
  ASSERT_TRUE(fs.store_file("normal.dat", data));
  ASSERT_TRUE(fs.read_file("normal.dat").has_value());
}

// T13: 分块哈希一致性（同一文件同一偏移稳定一致）
TEST(FileSystemTest, ComputeChunkHashConsistency) {
  TempDir tmp;
  std::string content(1000, 'z');
  tmp.write_file("big.bin", content);

  hps::FileSystem fs(tmp.path());
  auto chunks = fs.split_file(tmp.file_path("big.bin"), 256);
  ASSERT_GT(chunks.size(), 1U);

  std::string hash1 = fs.compute_chunk_hash(chunks[1]);
  std::string hash2 = fs.compute_chunk_hash(chunks[1]);
  EXPECT_FALSE(hash1.empty());
  EXPECT_EQ(hash1, hash2);
}
