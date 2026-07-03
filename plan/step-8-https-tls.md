# Step 8：HTTPS/TLS (OpenSSL)

> **状态**：待开始
> **优先级**：P0

## 背景

音乐软件需要安全的网络传输，支持 HTTP + HTTPS 双模式，基于 OpenSSL 实现 TLS 加密。

## 功能点

| # | 功能点 | 优先级 | 说明 |
|---|--------|--------|------|
| F1 | **SSLContext 封装** | P0 | OpenSSL SSL_CTX 生命周期管理 |
| F2 | **TLS 握手** | P0 | SSL_accept / SSL_connect 封装 |
| F3 | **双模式支持** | P0 | 同一端口可同时接受 HTTP / HTTPS |
| F4 | **证书管理** | P1 | 证书文件加载、验证 |

## 接口设计（初步）

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

## 测试用例（预估）

| # | 说明 |
|---|------|
| T1 | SSLContext 创建与销毁 |
| T2 | HTTP → HTTPS 升级 |
