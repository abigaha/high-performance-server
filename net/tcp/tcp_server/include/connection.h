#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace hps {

/**
 * TCP 连接封装
 *
 * 管理单个客户端连接的生命周期，包含非阻塞读写缓冲区和状态管理。
 * 继承 enable_shared_from_this 以便异步操作中安全持有自身引用。
 */
class Connection : public std::enable_shared_from_this<Connection> {
public:
  using ptr = std::shared_ptr<Connection>;
  using Clock = std::chrono::steady_clock;

  /** 连接状态 */
  enum class State : uint8_t { CONNECTED, CLOSED };

  Connection(int fd, const sockaddr_in& addr);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  /** 底层 socket 文件描述符 */
  int fd() const { return fd_; }

  /** 客户端 IP 地址（字符串形式） */
  std::string client_ip() const;

  /** 客户端端口号 */
  uint16_t client_port() const;

  /**
   * 从 socket 非阻塞读取数据
   * @return >0 读取字节数; 0 对端关闭; -1 出错
   */
  ssize_t read_from_fd();

  /**
   * 将 write_buffer_ 中的数据非阻塞写入 socket（加锁版，线程安全）
   * @return >0 写入字节数; 0 无数据可写; -1 出错
   */
  ssize_t write_to_fd();

  /**
   * write_to_fd 的不加锁版本（N1-H：调用方须持有 write_mutex）
   * @return >0 写入字节数; 0 无数据可写; -1 出错
   */
  ssize_t write_to_fd_locked();

  /** 关闭连接 */
  void close();

  /** 写入缓冲区访问锁（N1-H：保护 write_buffer_ 跨线程并发写） */
  std::mutex& write_mutex() { return write_mutex_; }

  /** 读取缓冲区（可写引用） */
  std::string& read_buffer() { return read_buffer_; }

  /** 读取缓冲区（只读） */
  const std::string& read_buffer() const { return read_buffer_; }

  /** 写入缓冲区（可写引用，调用方须持有 write_mutex） */
  std::string& write_buffer() { return write_buffer_; }

  /** 写入缓冲区（只读） */
  const std::string& write_buffer() const { return write_buffer_; }

  /** 消费 read_buffer_ 前 bytes 字节 */
  void consume_read_buffer(size_t bytes);

  /** 当前连接状态 */
  State state() const { return state_; }

  /** 最近活动时间 */
  Clock::time_point last_active() const { return last_active_; }

  /** 更新最近活动时间为现在 */
  void update_active() { last_active_ = Clock::now(); }

private:
  int fd_{-1}; ///< socket 文件描述符

  struct sockaddr_in addr_ {}; ///< 客户端地址信息

  std::string read_buffer_;        ///< 读取缓冲区
  std::string write_buffer_;       ///< 写入缓冲区
  std::size_t write_offset_{0};    ///< 已发送偏移（N5-M：避免 erase O(n) 移动）
  mutable std::mutex write_mutex_; ///< 写缓冲区锁（N1-H：跨线程并发写保护）
  State state_{State::CONNECTED};  ///< 连接状态
  Clock::time_point last_active_;  ///< 最近活动时间戳
};

} // namespace hps
