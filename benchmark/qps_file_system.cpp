#include "file_system.h"
#include "qps_runner.hpp"

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string create_temp_file(std::size_t size) {
  char path[] = "/tmp/hps_qps_XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0)
    return "";
  std::vector<char> buf(size, 'a');
  ssize_t written = 0;
  while (written < static_cast<ssize_t>(size)) {
    ssize_t n = write(fd, buf.data() + written, size - static_cast<std::size_t>(written));
    if (n < 0)
      break;
    written += n;
  }
  close(fd);
  return std::string(path);
}

} // namespace

int main() {
  auto levels = hps::bench::default_qps_levels();

  // SHA256 on 1KB data (in-memory)
  {
    std::string data(1024, 'x');
    hps::bench::run_qps_steps("FileSystem sha256_hex 1KB", levels, [&data](int) {
      auto hash = hps::FileSystem::sha256_hex(data.data(), data.size());
      (void)hash;
    });
  }

  // Store + Read small file (per-thread path, 避免竞态)
  {
    std::vector<char> data(1024, 'x');
    hps::FileSystem fs("/tmp");
    for (int i = 0; i < 100; ++i) {
      std::string vpath = "qps_bench_file_" + std::to_string(i);
      fs.store_file(vpath, data);
      fs.delete_file(vpath);
    }
    hps::bench::run_qps_steps("FileSystem store+read 1KB", levels, [&fs, &data](int tid) {
      std::string vpath = "qps_bench_rw_" + std::to_string(tid);
      fs.store_file(vpath, data);
      auto read_back = fs.read_file(vpath);
      (void)read_back;
      fs.delete_file(vpath);
    });
  }

  return 0;
}
