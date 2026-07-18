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

  bool store_file_record(const FileRecord& record) override;
  std::optional<FileRecord> get_file_record(int64_t file_id) override;
  std::optional<FileRecord> get_file_record_by_hash(const std::string& hash) override;
  std::vector<FileRecord> search_files(const std::string& name_pattern, int offset, int limit) override;

  bool store_file_chunks(const std::vector<FileChunkRecord>& chunks) override;
  std::vector<FileChunkRecord> get_file_chunks(const std::string& file_hash) override;
  bool chunk_exists(const std::string& chunk_hash) override;

  std::optional<AuthUser> get_auth_user(const std::string& username) override;
  bool verify_password(const std::string& username, const std::string& password) override;

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
