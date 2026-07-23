#include "stream_download_utils.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hps {
namespace {

class MemoryFileSystem final : public IFileSystem {
public:
  std::unordered_map<std::string, std::vector<char>> files;
  std::vector<std::string> read_paths;

  std::vector<FileChunk> split_file([[maybe_unused]] const std::string& path,
                                    [[maybe_unused]] std::size_t chunk_size) override {
    return {};
  }

  std::string compute_file_hash([[maybe_unused]] const std::string& path) override { return {}; }

  std::string compute_chunk_hash([[maybe_unused]] const FileChunk& chunk) override { return {}; }

  bool store_file(const std::string& path, const std::vector<char>& data) override {
    files[path] = data;
    return true;
  }

  bool delete_file(const std::string& path) override { return files.erase(path) > 0; }

  std::optional<std::vector<char>> read_file(const std::string& path) override {
    read_paths.push_back(path);
    const auto file_it = files.find(path);
    if (file_it == files.end()) {
      return std::nullopt;
    }
    return file_it->second;
  }
};

std::vector<char> to_bytes(std::string_view value) {
  return {value.begin(), value.end()};
}

std::vector<FileChunkRecord> make_chunks() {
  return {
    FileChunkRecord{.chunk_hash = "first", .chunk_size = 3},
    FileChunkRecord{.chunk_hash = "second", .chunk_size = 4},
    FileChunkRecord{.chunk_hash = "third", .chunk_size = 3},
  };
}

MemoryFileSystem make_file_system() {
  MemoryFileSystem file_system;
  file_system.files["chunks/first"] = to_bytes("abc");
  file_system.files["chunks/second"] = to_bytes("defg");
  file_system.files["chunks/third"] = to_bytes("hij");
  return file_system;
}

} // namespace

TEST(StreamDownloadUtilsTest, ReadsCompleteFileAcrossChunks) {
  auto file_system = make_file_system();
  const auto chunks = make_chunks();
  std::string body;

  ASSERT_TRUE(read_stream_file(file_system, chunks, 10, body));
  EXPECT_EQ(body, "abcdefghij");

  const std::vector<std::string> expected_paths{"chunks/first", "chunks/second", "chunks/third"};
  EXPECT_EQ(file_system.read_paths, expected_paths);
}

TEST(StreamDownloadUtilsTest, ReadsHalfOpenRangeAcrossChunks) {
  auto file_system = make_file_system();
  const auto chunks = make_chunks();
  std::string body;

  ASSERT_TRUE(read_stream_range(file_system, chunks, 2, 8, body));
  EXPECT_EQ(body, "cdefgh");

  const std::vector<std::string> expected_paths{"chunks/first", "chunks/second", "chunks/third"};
  EXPECT_EQ(file_system.read_paths, expected_paths);
}

TEST(StreamDownloadUtilsTest, ReportsUnreadableChunk) {
  auto file_system = make_file_system();
  const auto chunks = make_chunks();
  file_system.files.erase("chunks/second");
  std::string body;

  EXPECT_FALSE(read_stream_file(file_system, chunks, 10, body));
  EXPECT_EQ(body, "abc");
}

TEST(StreamDownloadUtilsTest, BuildsSafeContentDisposition) {
  const std::string utf8_name = "\xE9\x9F\xB3\xE4\xB9\x90 100%.mp3";
  EXPECT_EQ(sanitize_ascii_filename(utf8_name), "______ 100%.mp3");
  EXPECT_EQ(encode_rfc5987_filename(utf8_name), "%E9%9F%B3%E4%B9%90%20100%25.mp3");
  EXPECT_EQ(build_attachment_content_disposition(utf8_name),
            "attachment; filename=\"______ 100%.mp3\"; filename*=UTF-8''%E9%9F%B3%E4%B9%90%20100%25.mp3");

  const std::string unsafe_name = "bad\"\\/\r\nname;100%.mp3";
  const auto unsafe_header = build_attachment_content_disposition(unsafe_name);
  EXPECT_EQ(sanitize_ascii_filename(unsafe_name), "bad_____name;100%.mp3");
  EXPECT_EQ(encode_rfc5987_filename(unsafe_name), "bad%22%5C%2F%0D%0Aname%3B100%25.mp3");
  EXPECT_EQ(unsafe_header.find('\r'), std::string::npos);
  EXPECT_EQ(unsafe_header.find('\n'), std::string::npos);
  EXPECT_EQ(unsafe_header,
            "attachment; filename=\"bad_____name;100%.mp3\"; filename*=UTF-8''bad%22%5C%2F%0D%0Aname%3B100%25.mp3");
  EXPECT_EQ(build_attachment_content_disposition(""), "attachment; filename=\"download\"; filename*=UTF-8''");
}

} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
