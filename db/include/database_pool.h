#pragma once

#include "iconnection.h"
#include "idatabase_pool.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace hps {

class DatabasePool : public IDatabasePool {
public:
  using ConnectionFactory = std::function<std::unique_ptr<IConnection>()>;

  explicit DatabasePool(ConnectionFactory factory = nullptr);
  ~DatabasePool() override;

  DatabasePool(const DatabasePool&) = delete;
  DatabasePool& operator=(const DatabasePool&) = delete;
  DatabasePool(DatabasePool&&) = delete;
  DatabasePool& operator=(DatabasePool&&) = delete;

  bool init(const DbConfig& config) override;
  void close() override;

  std::optional<User> get_user(int64_t user_id) override;
  bool create_user(const User& user) override;
  bool log_download(const DownloadLog& log) override;
  std::vector<DownloadLog> get_download_history(int64_t user_id) override;
  bool store_file_meta(const FileMeta& meta) override;
  std::optional<FileMeta> get_file_meta(const std::string& hash) override;

protected:
  std::unique_ptr<IConnection> get_connection();
  void release_connection(std::unique_ptr<IConnection> conn);

private:
  void do_close(); // 非虚，供析构和 close() 共用

  ConnectionFactory factory_;
  DbConfig config_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::unique_ptr<IConnection>> pool_;
  std::atomic<int> active_connections_{0};
  bool closed_{false};
};

} // namespace hps
