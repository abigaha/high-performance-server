#include "file_system.h"
#include "file_transfer.h"
#include "i_tcp_client.h"
#include "qps_runner.hpp"

#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kSmallFileSize = 4 * 1024;
constexpr std::size_t kLargeFileSize = 1024 * 1024;
constexpr std::size_t kChunkSize = 64 * 1024;

class TempWorkspace {
public:
  TempWorkspace() {
    const char* temp_dir = std::getenv("TMPDIR");
    const std::filesystem::path base = temp_dir != nullptr ? temp_dir : "/tmp";
    path_ = base / ("hps_qps_file_transfer_" + std::to_string(getpid()));
    std::filesystem::create_directories(path_);
  }

  ~TempWorkspace() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempWorkspace(const TempWorkspace&) = delete;
  TempWorkspace& operator=(const TempWorkspace&) = delete;

  [[nodiscard]] std::string path() const { return path_.string(); }

  [[nodiscard]] std::string file_path(const std::string& name) const { return (path_ / name).string(); }

private:
  std::filesystem::path path_;
};

class MockTcpClient final : public hps::ITcpClient {
public:
  MockTcpClient(std::string expected_payload, bool accept_send) :
      expected_payload_(std::move(expected_payload)), accept_send_(accept_send) {}

  bool connect_to_server() override {
    connected_ = true;
    return true;
  }

  void disconnect() override { connected_ = false; }

  bool send_message(const std::string& message) const override { return accept_send_ && message == expected_payload_; }

  bool receive_message(std::string& message, hps::ReadMode mode, uint32_t timeout_ms) override {
    static_cast<void>(message);
    static_cast<void>(mode);
    static_cast<void>(timeout_ms);
    return false;
  }

  bool is_connected() const override { return connected_; }

private:
  std::string expected_payload_;
  bool accept_send_;
  bool connected_{true};
};

bool validate_chunks(const std::vector<hps::FileChunk>& chunks) {
  if (chunks.size() != kLargeFileSize / kChunkSize) {
    return false;
  }

  std::size_t expected_offset = 0;
  for (const auto& chunk : chunks) {
    if (chunk.offset != expected_offset || chunk.size != kChunkSize) {
      return false;
    }
    expected_offset += chunk.size;
  }
  return expected_offset == kLargeFileSize;
}

} // namespace

int main() {
  try {
    TempWorkspace workspace;
    auto file_system = std::make_shared<hps::FileSystem>(workspace.path());
    const std::string small_payload(kSmallFileSize, 's');
    const std::vector<char> small_data(small_payload.begin(), small_payload.end());
    const std::vector<char> large_data(kLargeFileSize, 'l');

    if (!file_system->store_file("small.bin", small_data) || !file_system->store_file("large.bin", large_data)) {
      std::cerr << "无法创建 FileTransfer QPS 测试文件\n";
      return EXIT_FAILURE;
    }

    hps::FileTransfer transfer(file_system, kChunkSize);
    MockTcpClient accepting_client(small_payload, true);
    MockTcpClient rejecting_client(small_payload, false);
    const auto levels = hps::bench::default_qps_levels();

    hps::bench::run_qps_steps("FileTransfer small file 4KB mock send", levels, [&](int) {
      return transfer.transfer_small("small.bin", accepting_client);
    });

    const std::string large_path = workspace.file_path("large.bin");
    hps::bench::run_qps_steps("FileTransfer large file 1MB chunk planning", levels, [&](int) {
      return validate_chunks(file_system->split_file(large_path, kChunkSize));
    });

    hps::bench::run_qps_steps("FileTransfer rejected send validation", levels, [&](int) {
      return !transfer.transfer_small("small.bin", rejecting_client);
    });

    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "FileTransfer QPS 测试异常：" << error.what() << '\n';
  } catch (...) {
    std::cerr << "FileTransfer QPS 测试发生未知异常\n";
  }
  return EXIT_FAILURE;
}
