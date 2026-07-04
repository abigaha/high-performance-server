# Step 8：HTTPS/TLS (OpenSSL)

> **状态**：✅ 已完成
> **优先级**：P0

## 背景

音乐软件需要安全的网络传输，支持 HTTP + HTTPS 双模式，基于 OpenSSL 实现 TLS 加密。

## 功能点

| # | 功能点 | 优先级 | 说明 | 状态 |
|---|--------|--------|------|------|
| F1 | **SslContext 封装** | P0 | SSL_CTX 生命周期管理（前向声明避免 OpenSSL 头泄漏） | ✅ |
| F2 | **TLS 握手** | P0 | 异步 SSL_accept（WANT_READ/WANT_WRITE epoll 重试） | ✅ |
| F3 | **双模式支持** | P0 | 同一端口 peek 首字节 0x16 → TLS，否则明文 | ✅ |
| F4 | **证书管理** | P1 | 证书文件加载验证 + xmake after_build 自动生成自签名证书 | ✅ |

## 接口设计（最终）

```cpp
struct SslConfig {
  std::string cert_file;
  std::string key_file;
  bool verify_peer{false};
};

class SslContext {
public:
  explicit SslContext(const SslConfig& config);
  ~SslContext();
  SSL* create_ssl();
  bool accept(SSL* ssl, int fd);
  ssize_t read(SSL* ssl, void* buf, std::size_t len);
  ssize_t write(SSL* ssl, const void* buf, std::size_t len);
};
```

## 文件清单（预估）

| 操作 | 路径 |
|------|------|
| 创建 | `net/ssl/include/ssl_context.h` |
| 创建 | `net/ssl/src/ssl_context.cpp` |
| 创建 | `net/ssl/xmake.lua` |
| 修改 | `net/tcp/tcp_server/include/tcp_server.h`（SSL 支持）|
| 修改 | `net/tcp/tcp_server/src/tcp_server.cpp`（SSL 集成）|

## 测试用例

| # | 说明 | 状态 |
|---|------|------|
| T1 | SslContext 创建与销毁 | ✅ |
| T2 | 无效证书抛异常 | ✅ |
| T3 | create_ssl 返回非空 | ✅ |
| T4 | TLS 握手 + Echo 通信 | ✅ |
| T5 | SSL 加密读写 | ✅ |
| T6 | 双模式——明文 HTTP | ✅ |
| T7 | 双模式——HTTPS 请求 | ✅ |
| T8 | Connection SSL 清理 | ✅ |
| T9 | 未启用 SSL 时零开销 | ✅ |
