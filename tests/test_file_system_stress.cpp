#include "file_system.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using namespace hps;

namespace {

class TempDir {
public:
  TempDir() {
    auto* tmpdir = std::getenv("TMPDIR");
    std::string base = (tmpdir != nullptr) ? tmpdir : "/tmp";
    path_ = base + "/hps_test_fs_stress_" + std::to_string(getpid());
    fs::create_directories(path_);
  }

  ~TempDir() { fs::remove_all(path_); }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

  std::string file_path(const std::string& name) const { return path_ + "/" + name; }

  void write_file(const std::string& name, const std::string& content) const {
    std::ofstream f(file_path(name), std::ios::binary);
    f << content;
  }

private:
  std::string path_;
};

std::string make_string(std::size_t size, char c) {
  // NOLINTNEXTLINE(modernize-return-braced-init-list)
  return std::string(size, c);
}

} // namespace

TEST(FileSystemStressTest, ConcurrentReadWrite) {
  TempDir tmp;
  FileSystem fs(tmp.path());

  constexpr int kThreads = 4;
  constexpr int kFilesPerThread = 10;
  std::atomic<int> success_count{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kFilesPerThread; ++i) {
        std::string name = "thread_" + std::to_string(t) + "_file_" + std::to_string(i) + ".dat";
        std::vector<char> data(1024, static_cast<char>('A' + t));
        if (fs.store_file(name, data)) {
          auto read_back = fs.read_file(name);
          if (read_back.has_value() && read_back->size() == data.size() && (*read_back)[0] == data[0]) {
            success_count.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  EXPECT_EQ(success_count.load(), kThreads * kFilesPerThread);
}

TEST(FileSystemStressTest, StoreAndReadLargeFile) {
  TempDir tmp;
  FileSystem fs(tmp.path());

  std::size_t size = 1024 * 1024;
  std::vector<char> data(size, 'X');

  ASSERT_TRUE(fs.store_file("large.dat", data));

  auto read_back = fs.read_file("large.dat");
  ASSERT_TRUE(read_back.has_value());
  EXPECT_EQ(read_back->size(), size);
  EXPECT_EQ((*read_back)[0], 'X');
  EXPECT_EQ((*read_back)[size - 1], 'X');
}

TEST(FileSystemStressTest, SplitLargeFile) {
  TempDir tmp;
  std::string content = make_string(5 * 1024 * 1024, 'Y');
  tmp.write_file("big.bin", content);

  FileSystem fs(tmp.path());
  auto chunks = fs.split_file(tmp.file_path("big.bin"), 1024 * 1024);

  ASSERT_EQ(chunks.size(), 5);
  EXPECT_EQ(chunks[0].offset, 0U);
  EXPECT_EQ(chunks[0].size, 1024 * 1024U);
  EXPECT_EQ(chunks[4].offset, 4U * 1024 * 1024);
  EXPECT_EQ(chunks[4].size, 1024 * 1024U);
}

TEST(FileSystemStressTest, ReadNonExistentFile) {
  TempDir tmp;
  FileSystem fs(tmp.path());

  auto result = fs.read_file("nonexistent_file.dat");
  EXPECT_FALSE(result.has_value());
}

TEST(FileSystemStressTest, DeleteNonExistentFile) {
  TempDir tmp;
  FileSystem fs(tmp.path());

  EXPECT_FALSE(fs.delete_file("nonexistent_file.dat"));
}

TEST(FileSystemStressTest, StoreOverwrite) {
  TempDir tmp;
  FileSystem fs(tmp.path());

  std::vector<char> data1(100, 'A');
  ASSERT_TRUE(fs.store_file("overwrite.dat", data1));

  std::vector<char> data2(200, 'B');
  ASSERT_TRUE(fs.store_file("overwrite.dat", data2));

  auto read_back = fs.read_file("overwrite.dat");
  ASSERT_TRUE(read_back.has_value());
  EXPECT_EQ(read_back->size(), 200U);
  EXPECT_EQ((*read_back)[0], 'B');
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
