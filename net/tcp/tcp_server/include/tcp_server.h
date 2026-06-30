#pragma once

#include "connection.h"

#include <sys/epoll.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hps {

class ThreadPool;

/**
 * TCP 服务器
 *
 * 基于 epoll ET（边缘触发）模式 + ThreadPool 的高性能 TCP 服务器。
 * 支持优雅关闭（SIGINT/SIGTERM）、跨线程事件通知（eventfd）、
 * 可配置线程数/端口/backlog/超时等参数。
 */
class TcpServer {
public:
  /** 服务器配置 */
  struct Config {
    uint16_t port{8080};       ///< 监听端口（0 表示由内核自动分配）
    size_t backlog{128};       ///< listen backlog 队列长度
    size_t thread_count{4};    ///< ThreadPool 工作线程数
    int epoll_timeout_ms{100}; ///< epoll_wait 超时（毫秒）
  };

  /** 连接处理器类型：接收一个 Connection 智能指针 */
  using Handler = std::function<void(std::shared_ptr<Connection>)>;

  /**
   * 默认构造函数
   *
   * 使用 Config 默认值初始化。注意：不能直接写成
   * TcpServer(Config config = {})，因为 GCC 对含有默认成员初始化器的
   * 聚合类型的默认参数存在兼容问题。
   */
  TcpServer() : config_(Config{}) {}

  /** 使用自定义配置构造 */
  explicit TcpServer(const Config& config);
  ~TcpServer();

  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  /** 初始化：socket/bind/listen/epoll/eventfd/signal/ThreadPool */
  bool init();

  /** 启动事件循环（阻塞，由主线程调用） */
  void start();

  /** 停止服务器（线程安全，可被信号处理函数调用） */
  void stop();

  /** 设置连接处理器 */
  void set_handler(Handler handler);

  /** 服务器是否正在运行 */
  bool is_running() const { return running_.load(); }

  /** 当前连接数 */
  size_t connection_count() const { return connections_.size(); }

  /** 实际绑定的端口（port=0 自动分配时有效） */
  uint16_t actual_port() const { return actual_port_; }

  /** 通知事件循环唤醒（跨线程） */
  static void notify_wake(int wake_fd);

private:
  /** 主事件循环 */
  void event_loop();

  /** 处理单次 epoll 事件分发 */
  void handle_event(const struct epoll_event& evt);

  /** 处理新连接（accept4） */
  bool handle_accept();

  /** 处理可读事件 */
  bool handle_read(int fd);

  /** 处理可写事件 */
  bool handle_write(int fd);

  /** 关闭连接并从 epoll 中移除 */
  void close_connection(int fd);

  /** 处理被 handler 线程标记为 dirty 的连接 */
  void process_dirty_connections();

  /** 清理所有资源（epoll/fd/signal） */
  void cleanup_resources();

  Config config_;                    ///< 服务器配置
  uint16_t actual_port_{0};          ///< 实际绑定端口
  int server_sockfd_{-1};            ///< 监听 socket fd
  int epoll_fd_{-1};                 ///< epoll 实例 fd
  int wake_fd_{-1};                  ///< eventfd（跨线程唤醒事件循环）
  std::atomic<bool> running_{false}; ///< 运行状态标志

  std::unique_ptr<ThreadPool> thread_pool_;                          ///< 工作线程池
  std::unordered_map<int, std::shared_ptr<Connection>> connections_; ///< 活跃连接表
  Handler handler_;                                                  ///< 连接处理器

  // 跨线程通知：handler (ThreadPool) → event loop
  std::mutex dirty_mutex_;     ///< dirty_fds_ 保护锁
  std::vector<int> dirty_fds_; ///< 待处理 EPOLLOUT 的连接 fd

  static TcpServer* s_instance_;       ///< 全局实例指针（信号处理用）
  static void signal_handler(int sig); ///< SIGINT/SIGTERM 处理函数
};

} // namespace hps
