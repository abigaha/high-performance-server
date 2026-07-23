#pragma once

#include "connection.h"
#include "i_tcp_server.h"
#include "ssl_context.h"
#include "thread_pool.h"

#include <sys/epoll.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hps {

/**
 * TCP 服务器
 *
 * 基于 epoll ET（边缘触发）模式 + LockFreeThreadPool 的高性能 TCP 服务器。
 * 支持优雅关闭（SIGINT/SIGTERM）、跨线程事件通知（eventfd）、
 * 可配置线程数/端口/backlog/超时等参数。
 */
class TcpServer : public ITcpServer {
public:
  using CloseHandler = std::function<void(Connection*)>;

  /** 服务器配置 */
  struct Config {
    uint16_t port{8080};       ///< 监听端口（0 表示由内核自动分配）
    size_t backlog{128};       ///< listen backlog 队列长度
    size_t thread_count{4};    ///< LockFreeThreadPool 工作线程数
    int epoll_timeout_ms{100}; ///< epoll_wait 超时（毫秒）
    SslConfig ssl_config{};    ///< SSL 配置（enabled=false 时不启用）
  };

  /**
   * 默认构造函数
   *
   * 使用 Config 默认值初始化。注意：不能直接写成
   * TcpServer(Config config = {})，因为 GCC 对含有默认成员初始化器的
   * 聚合类型的默认参数存在兼容问题。
   */
  TcpServer() : TcpServer(Config{}) {}

  /** 使用自定义配置构造 */
  explicit TcpServer(const Config& config);
  ~TcpServer() override;

  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  bool init() override;
  void start() override;
  void stop() override;
  void set_handler(Handler handler) override;

  void set_close_handler(CloseHandler handler) { close_handler_ = std::move(handler); }

  uint16_t actual_port() const override { return actual_port_; }

  /** 服务器是否正在运行 */
  bool is_running() const { return running_.load(); }

  /** 当前连接数 */
  size_t connection_count() const { return connections_.size(); }

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

  /** SSL 握手处理 */
  void handle_ssl_handshake(std::shared_ptr<Connection>& conn, const struct epoll_event& ev);

  /** SSL 双模式检测（首次 EPOLLIN 时 peek 首字节判断协议） */
  bool try_ssl_detection(std::shared_ptr<Connection>& conn, const struct epoll_event& ev);

  /** SSL 握手重试 */
  bool try_ssl_handshake(std::shared_ptr<Connection>& conn, const struct epoll_event& ev);

  /** 处理普通连接事件（非 SSL 状态） */
  void handle_client_event(int fd, const struct epoll_event& ev);

  /** 清理所有资源（epoll/fd/signal） */
  void cleanup_resources();

  Config config_;
  std::unique_ptr<SslContext> ssl_context_; ///< SSL 上下文（可选）                    ///< 服务器配置
  uint16_t actual_port_{0};                 ///< 实际绑定端口
  int server_sockfd_{-1};                   ///< 监听 socket fd
  int epoll_fd_{-1};                        ///< epoll 实例 fd
  int wake_fd_{-1};                         ///< eventfd（跨线程唤醒事件循环）
  std::atomic<bool> running_{false};        ///< 运行状态标志

  std::unique_ptr<LockFreeThreadPool> thread_pool_;                  ///< 工作线程池
  std::unordered_map<int, std::shared_ptr<Connection>> connections_; ///< 活跃连接表
  Handler handler_;                                                  ///< 连接处理器
  CloseHandler close_handler_;                                       ///< 连接关闭通知

  // 跨线程通知：handler (LockFreeThreadPool) → event loop
  std::mutex dirty_mutex_;     ///< dirty_fds_ 保护锁
  std::vector<int> dirty_fds_; ///< 待处理 EPOLLOUT 的连接 fd

  static TcpServer* s_instance_;       ///< 全局实例指针（信号处理用）
  static void signal_handler(int sig); ///< SIGINT/SIGTERM 处理函数
};

} // namespace hps
