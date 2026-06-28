# 开发计划

> 基于 `goal.md` 模块优先级，分阶段实现音乐软件 HTTP 服务器。

---

## 总模块顺序

| 阶段 | 模块 | 状态 |
|------|------|------|
| **Module 0** | **CTcpServer 重构优化** | **← 当前** |
| **Module 1** | **HTTP 请求/响应基本类型** | **← 当前（紧随 Module 0）** |
| Module 2 | 路由注册 + 请求分发 | 待开始 |
| Module 3 | file-system 实现 + Range 流式传输 | 待开始 |
| Module 4 | MySQL 连接池 + 数据模型 | 待开始 |
| Module 5 | HTTPS/TLS | 待开始 |
| Module 6 | WebSocket | 待开始 |
| Module 7 | file-send / file-receive | 待开始 |
| Module 8 | main.cpp 整合启动 | 待开始 |

---

## Module 0: CTcpServer 重构优化

### 背景

当前 `CTcpServer` 存在严重缺陷：
1. **单连接** — `client_sockfd_` 只有一个，accept 完一个才能接下一个
2. **阻塞 I/O** — `accept`/`recv`/`send` 全阻塞
3. **backlog 写死 5** — 无法配置
4. **未复用 ThreadPool** — 串行处理
5. **未复用 Coroutine** — 无异步流程
6. **无优雅关闭** — 析构直接 close

### 架构设计

```
┌──────────────────────────────────────────────────────────┐
│                    CTcpServer                             │
│                                                           │
│  ┌──────────────────┐     ┌───────────────────────────┐  │
│  │   EventLoop       │     │       ThreadPool           │  │
│  │  (主线程)          │─────►  (worker 线程)            │  │
│  │  epoll_wait       │◄────│  ┌─────────────────────┐  │  │
│  │  + dispatch       │     │  │ CoroItem<handler>   │  │  │
│  └────────┬─────────┘     │  │ per connection       │  │  │
│           │               │  └─────────────────────┘  │  │
│           ▼               └───────────────────────────┘  │
│  ┌────────────────────────────────────┐                   │
│  │      ConnectionManager             │                   │
│  │  ┌──────┐ ┌──────┐ ┌──────┐      │                   │
│  │  │ Conn │ │ Conn │ │ Conn │ ...  │                   │
│  │  │fd=7  │ │fd=9  │ │fd=11 │      │                   │
│  │  └──────┘ └──────┘ └──────┘      │                   │
│  └────────────────────────────────────┘                   │
└──────────────────────────────────────────────────────────┘
```

### 功能点

| # | 功能点 | 优先级 | 说明 |
|---|--------|--------|------|
| F0 | **Connection 类** | P0 | 封装单连接状态：fd、地址、读写缓冲区、生命周期 |
| F1 | **epoll 事件驱动** | P0 | 非阻塞 socket + epoll ET 边缘触发，多连接并发 |
| F2 | **ThreadPool 集成** | P0 | accept 后的数据处理通过 ThreadPool 分发 |
| F3 | **CoroItem 协程集成** | P0 | 每个连接的 handler 运行在 CoroItem 中 |
| F4 | **可配置参数** | P0 | port、backlog、thread_count、epoll_timeout |
| F5 | **优雅关闭** | P0 | SIGINT/SIGTERM → stop accept → 逐连接关闭 |
| F6 | **错误处理** | P1 | 错误日志，异常 fd 自动清理 |

### 接口设计

#### Connection (`net/tcp/ctcpserver/include/connection.h`)

```cpp
namespace hps {

class Connection : public std::enable_shared_from_this<Connection> {
public:
  using ptr = std::shared_ptr<Connection>;
  using Clock = std::chrono::steady_clock;
  enum class State : uint8_t { CONNECTED, CLOSED };

  Connection(int fd, const sockaddr_in& addr);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  int fd() const { return fd_; }
  std::string client_ip() const;
  uint16_t client_port() const;

  ssize_t read_from_fd();
  ssize_t write_to_fd();
  void close();

  std::string& read_buffer() { return read_buffer_; }
  const std::string& read_buffer() const { return read_buffer_; }
  std::string& write_buffer() { return write_buffer_; }
  const std::string& write_buffer() const { return write_buffer_; }

  void consume_read_buffer(size_t bytes);
  State state() const { return state_; }
  Clock::time_point last_active() const { return last_active_; }
  void update_active() { last_active_ = Clock::now(); }

private:
  int fd_{-1};
  struct sockaddr_in addr_{};
  std::string read_buffer_;
  std::string write_buffer_;
  State state_{State::CONNECTED};
  Clock::time_point last_active_;
};

} // namespace hps
```

#### CTcpServer (`net/tcp/ctcpserver/include/ctcpserver.h`)

```cpp
namespace hps {

class ThreadPool;

class CTcpServer {
public:
  struct Config {
    uint16_t port{8080};
    size_t backlog{128};
    size_t thread_count{4};
    int epoll_timeout_ms{100};
  };

  using Handler = std::function<void(std::shared_ptr<Connection>)>;

  explicit CTcpServer(Config config = {});
  ~CTcpServer();
  CTcpServer(const CTcpServer&) = delete;
  CTcpServer& operator=(const CTcpServer&) = delete;

  bool init();
  void start();
  void stop();
  void set_handler(Handler handler);
  bool is_running() const { return running_.load(); }
  size_t connection_count() const { return connections_.size(); }

private:
  void event_loop();
  bool handle_accept();
  bool handle_read(int fd);
  bool handle_write(int fd);
  void close_connection(int fd);
  bool set_non_blocking(int fd) const;

  Config config_;
  int server_sockfd_{-1};
  int epoll_fd_{-1};
  std::atomic<bool> running_{false};
  std::unique_ptr<ThreadPool> thread_pool_;
  std::unordered_map<int, std::shared_ptr<Connection>> connections_;
  Handler handler_;
  static CTcpServer* s_instance;
  static void signal_handler(int sig);
};

} // namespace hps
```

### 连接生命周期

```
CLOSED → init() → LISTENING → accept() → CONNECTED
  │                                           │
  │                                    [EPOLLIN] → READING
  │                                           │
  │                                    ThreadPool enqueue
  │                                           │
  │                                    PROCESSING (CoroItem)
  │                                           │
  │                                    write_buffer ready
  │                                           │
  │                                    [EPOLLOUT] → WRITING
  │                                           │
  │                                   ┌───────┴───────┐
  │                                keep-alive        close
  │                                   │                │
  │                                   ▼                ▼
  │                               READING          CLOSING → CLOSED
  └─────────────────────────────────────────────────────────┘
```

### 文件清单

| 操作 | 路径 |
|------|------|
| 创建 | `net/tcp/ctcpserver/include/connection.h` |
| 创建 | `net/tcp/ctcpserver/src/connection.cpp` |
| 重写 | `net/tcp/ctcpserver/include/ctcpserver.h` |
| 重写 | `net/tcp/ctcpserver/src/ctcpserver.cpp` |
| 创建 | `tests/test_ctcpserver_connection.cpp` |
| 创建 | `tests/test_ctcpserver.cpp` |

### 破坏性变更

| 旧接口 | 操作 | 替代 |
|--------|------|------|
| `acceptClient()` | 移除 | 事件循环内部处理 |
| `sendMessage()` | 移除 | `Connection::write_buffer_` + epoll EPOLLOUT |
| `receiveMessage()` | 移除 | `Connection::read_buffer_` + handler 回调 |
| `closeClient()` | 移除 | `Connection::close()` |
| `closeListen()` | 移除 | `CTcpServer::stop()` 内部处理 |

---

## Module 1: HTTP 请求/响应基本类型

### 背景

在重构后的 CTcpServer 之上构建 HTTP 协议层。提供 `HttpRequest`/`HttpResponse` 数据结构和 HTTP 请求解析器，为后续路由模块提供基础。

### 已确定的设计决策

| # | 事项 | 决定 |
|---|------|------|
| 1 | Headers 大小写策略 | **方案 C**：自定义 `CaseInsensitiveHash` + `CaseInsensitiveEq` |
| 2 | 解析器模式 | **流式增量解析**（状态机），`add_data()` + `parse()` 分离 |
| 3 | Body 最大大小 | **100 MiB**，超出返回 `413 Payload Too Large` |
| 4 | Content-Encoding | **第一阶段不处理**，透传原始 body |
| 5 | Transfer-Encoding: chunked | **实现**：解析 chunk 块 |
| 6 | 解析错误码 | 维持粗粒度（OK/INCOMPLETE/BAD_REQUEST/PAYLOAD_TOO_LARGE） |

### 功能点

| # | 功能点 | 优先级 | 说明 |
|---|--------|--------|------|
| F1 | **大小写不敏感 HeaderMap** | P0 | 自定义 hash/eq 仿函数 |
| F2 | **HttpRequest 数据结构** | P0 | 方法、URL、版本、headers、body |
| F3 | **HttpResponse 数据结构** | P0 | 版本、状态码、headers、body + 序列化 |
| F4 | **HTTP 请求解析器（流式状态机）** | P0 | 状态机：请求行 → headers → body(chunked/identity) |
| F5 | **URL 解码** | P0 | 百分号编码解码 |
| F6 | **Chunked Transfer-Encoding** | P0 | 解析 chunked 请求体 |
| F7 | **Body 大小限制** | P0 | 100 MiB 上限，超出返回 413 |

### 接口设计

#### 大小写不敏感工具 (`net/http/include/case_insensitive.h`)

```cpp
namespace hps {

struct CaseInsensitiveHash {
  size_t operator()(const std::string& key) const;
};

struct CaseInsensitiveEq {
  bool operator()(const std::string& lhs, const std::string& rhs) const;
};

using HeaderMap = std::unordered_map<
  std::string, std::string,
  CaseInsensitiveHash, CaseInsensitiveEq>;

} // namespace hps
```

#### HttpRequest (`net/http/include/http_request.h`)

```cpp
namespace hps {

enum class HttpMethod { GET, POST, PUT, DELETE, HEAD, OPTIONS, PATCH, UNKNOWN };

class HttpRequest {
public:
  HttpMethod method{HttpMethod::GET};
  std::string url;
  std::string path;
  std::string query_string;
  std::string http_version{"HTTP/1.1"};
  HeaderMap headers;
  std::string body;

  bool has_header(const std::string& key) const;
  std::string get_header(const std::string& key) const;
  void set_header(const std::string& key, const std::string& value);
  std::unordered_map<std::string, std::string> parse_query() const;
  size_t content_length() const;
  std::string method_string() const;
  static HttpMethod parse_method(const std::string& m);
};

} // namespace hps
```

#### HttpResponse (`net/http/include/http_response.h`)

```cpp
namespace hps {

enum class HttpStatusCode {
  OK = 200, CREATED = 201, NO_CONTENT = 204,
  MOVED_PERMANENTLY = 301, FOUND = 302, NOT_MODIFIED = 304,
  BAD_REQUEST = 400, UNAUTHORIZED = 401, FORBIDDEN = 403,
  NOT_FOUND = 404, METHOD_NOT_ALLOWED = 405,
  PAYLOAD_TOO_LARGE = 413,
  INTERNAL_SERVER_ERROR = 500, BAD_GATEWAY = 502, SERVICE_UNAVAILABLE = 503,
};

class HttpResponse {
public:
  std::string http_version{"HTTP/1.1"};
  HttpStatusCode status_code{HttpStatusCode::OK};
  std::string status_message{"OK"};
  HeaderMap headers;
  std::string body;

  void set_status(HttpStatusCode code, const std::string& message = "");
  bool has_header(const std::string& key) const;
  std::string get_header(const std::string& key) const;
  void set_header(const std::string& key, const std::string& value);
  void set_content_type(const std::string& type);
  size_t content_length() const;
  std::string to_string() const;
};

} // namespace hps
```

#### HTTP 请求解析器 (`net/http/include/http_parser.h`)

```cpp
namespace hps {

constexpr size_t MAX_BODY_SIZE = 100 * 1024 * 1024; // 100 MiB

enum class ParseResult { OK, INCOMPLETE, BAD_REQUEST, PAYLOAD_TOO_LARGE };

struct ParseError {
  ParseResult result{ParseResult::OK};
  std::string message;
};

enum class ParserState {
  REQUEST_LINE, HEADERS,
  BODY_IDENTITY, BODY_CHUNKED, BODY_CHUNK_EXT,
  BODY_CHUNK_DATA, BODY_CHUNK_TRAILER,
  COMPLETE, ERROR,
};

class HttpRequestParser {
public:
  HttpRequestParser();

  void add_data(const std::string& data);
  ParseError parse(HttpRequest& request);
  void reset();
  bool is_complete() const;
  bool is_chunked() const;

private:
  static constexpr size_t MAX_HEADER_SIZE = 64 * 1024;
  static constexpr size_t MAX_LINE_SIZE = 8 * 1024;

  ParserState state_{ParserState::REQUEST_LINE};
  std::string buffer_;
  size_t consumed_{0};
  size_t chunk_remaining_{0};

  ParseError parse_request_line(HttpRequest& request);
  ParseError parse_headers(HttpRequest& request);
  ParseError parse_body_identity(HttpRequest& request);
  ParseError parse_body_chunked(HttpRequest& request);
  std::optional<std::string> extract_line();
  std::optional<std::string> extract_data(size_t length);
  static size_t decode_chunk_size(const std::string& hex_str);
};

} // namespace hps
```

#### URL 解码 (`net/http/include/url_decode.h`)

```cpp
namespace hps {
std::string url_decode(const std::string& encoded);
} // namespace hps
```

### Chunked 解码流程

```
chunked-body = *chunk last-chunk trailer-part CRLF
chunk       = chunk-size [ chunk-ext ] CRLF chunk-data CRLF
chunk-size  = 1*HEXDIG
last-chunk  = 1*("0") [ chunk-ext ] CRLF
chunk-data  = 1*OCTET
trailer-part = *( header-field CRLF )
```

### 文件清单

| 操作 | 路径 |
|------|------|
| 创建 | `net/http/include/case_insensitive.h` |
| 创建 | `net/http/include/http_request.h` |
| 创建 | `net/http/include/http_response.h` |
| 创建 | `net/http/include/http_parser.h` |
| 创建 | `net/http/include/url_decode.h` |
| 创建 | `net/http/src/http_request.cpp` |
| 创建 | `net/http/src/http_response.cpp` |
| 创建 | `net/http/src/http_parser.cpp` |
| 创建 | `net/http/src/url_decode.cpp` |
| 创建 | `net/http/xmake.lua` |
| 修改 | `net/xmake.lua` (添加 `includes("http")`) |
| 创建 | `tests/test_http_request.cpp` |
| 创建 | `tests/test_http_response.cpp` |
| 创建 | `tests/test_http_parser.cpp` |
| 创建 | `tests/test_url_decode.cpp` |

---

## 质量门禁（每模块严格执行）

- [ ] clang-tidy: 0 error + 0 warning + 0 style
- [ ] cppcheck --enable=all: 0 error + 0 warning + 0 style + 0 performance
- [ ] 编译: 0 error + 0 warning
- [ ] xmake test: 100% 通过
- [ ] CodeQL: 0 critical + 0 high severity
