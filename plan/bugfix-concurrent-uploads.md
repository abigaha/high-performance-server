# Bug 修复：大并发上传堆内存崩溃 + Keep-Alive 上传请求污染

## Bug 1：`read_buffer_` 无锁跨线程访问 → malloc 堆内存损坏

### 现象

高并发 10MB 上传（2000+ 并发）时服务器 crash，错误信息：

```
malloc(): unsorted double linked list corrupted
```

### 根因分析

`Connection::read_buffer_` 在**事件循环线程**和 **Handler 线程**之间无锁并发访问：

| 线程 | 函数 | 操作 |
|------|------|------|
| 事件循环（epoll ET） | `plain_read_from_fd()` | `read_buffer_.append(...)` 追加数据 |
| Handler（线程池） | `handle_connection()` | `conn.read_buffer()` 读取 + `consume_read_buffer()` 擦除 |

`std::string::append` 在追加数据时可能触发内部缓冲区**重新分配**（realloc），释放旧内存块并分配更大的新块。此时若 Handler 线程已持有通过 `conn.read_buffer().data()` 获取的指针/`string_view`，该指针指向的旧内存已被释放 → 后续读写该内存时破坏 malloc 的 free list → `malloc(): unsorted double linked list corrupted`。

高并发上传场景下该 race 更容易触发：

1. 10MB 上传需要多次 TCP read 事件（事件循环反复追加到 `read_buffer_`）
2. Handler 做文件 I/O 写入 chunk 时持有 `conn_mutex`，但 `read_buffer_` 不受该 mutex 保护
3. 事件循环在 Handler 处理期间可能因新 EPOLLIN 继续追加数据

### 受影响的文件

| 文件 | 行号 | 问题 |
|------|------|------|
| `net/tcp/tcp_server/src/connection.cpp` | `plain_read_from_fd():84` | `read_buffer_.append()` 无锁 |
| `net/tcp/tcp_server/src/connection.cpp` | `ssl_read_from_fd():54` | `read_buffer_.append()` 无锁 |
| `net/tcp/tcp_server/src/connection.cpp` | `consume_read_buffer():205-213` | `read_buffer_.erase()` 无锁 |
| `net/http/src/http_server.cpp` | `handle_connection():177` | `const auto& buf = conn.read_buffer()` 无锁读取 |

### 修复方案

**方案：** 在 `Connection` 中添加 `read_mutex_`，所有对 `read_buffer_` 的写操作和读操作均加锁保护。

#### 1.1 新增互斥量

**文件：** `net/tcp/tcp_server/include/connection.h:131`

```cpp
mutable std::mutex read_mutex_;  ///< 读缓冲区锁
std::string read_buffer_;
```

对外暴露 `read_mutex()` 访问器供 Handler 层使用。

#### 1.2 `plain_read_from_fd` / `ssl_read_from_fd` 加锁追加

**文件：** `net/tcp/tcp_server/src/connection.cpp:84`

```cpp
{
  std::lock_guard<std::mutex> lock(read_mutex_);
  read_buffer_.append(t_buf.data(), static_cast<size_t>(n));
}
```

#### 1.3 `consume_read_buffer` 加锁擦除 + split locked/nolocked

**文件：** `net/tcp/tcp_server/src/connection.cpp:205-213`

```cpp
void Connection::consume_read_buffer(size_t bytes) {
  std::lock_guard<std::mutex> lock(read_mutex_);
  consume_read_buffer_locked(bytes);
}

void Connection::consume_read_buffer_locked(size_t bytes) {
  // 调用方必须已持有 read_mutex_
  if (bytes >= read_buffer_.size()) {
    read_buffer_.clear();
  } else {
    read_buffer_.erase(0, bytes);
  }
  update_active();
}
```

#### 1.4 `handle_connection` 拷贝缓冲区避免持锁操作

**文件：** `net/http/src/http_server.cpp:179-183`

将缓冲区拷贝到局部变量后释放锁，避免锁跨越 WebSocket 等阻塞操作：

```cpp
std::string buf;
{
  std::lock_guard<std::mutex> rlock(conn.read_mutex());
  buf = conn.read_buffer();
}
// 之后所有解析操作基于 buf 副本，无需持锁
```

#### 1.5 `ws_connection.cpp` 同步加保护

**文件：** `net/websocket/src/ws_connection.cpp:119-123`

WebSocket 的独立 poll 循环也需要保护 `read_buffer_`：

```cpp
std::string local_buf;
{
  std::lock_guard<std::mutex> rlock(conn_->read_mutex());
  local_buf = conn_->read_buffer();
  conn_->consume_read_buffer_locked(local_buf.size());
}
```

---

## Bug 2：Keep-Alive 多请求场景下 `upload_ctx` 复用污染

### 现象

Keep-Alive 连接中连续发送多个请求时，后一个请求的 `UploadStreamContext` 会误用到前一个请求的残留数据（chunks、hash_ctx），导致：

- 上传文件元数据污染（重复插入 DB）
- `hash_ctx` 为 nullptr 时 upload handler 解引用崩溃

### 触发条件

1. 客户端在单个 TCP 连接中 pipeline 两个请求（Keep-Alive）
2. 第一个请求是 `POST /api/files/upload`（流式上传）
3. 第二个请求是另一个 upload 或普通 GET 请求

### 根因分析

`handle_connection` 中 `upload_ctx` 在 `while` 循环外创建（`http_server.cpp:185`），而 `parser.reset()` 在请求处理完成后清除了 `headers_done_cb_` 和 `chunk_handler_`，但 `upload_ctx` 及其内部数据（`chunks` 向量、`hash_ctx`）**未被重置**。

处理完第一个上传请求后的残留状态：

| 字段 | 残留值 | 对第二个请求的影响 |
|------|--------|-------------------|
| `chunks` | 第一个请求的分片记录 | 非空 → `if (!upload_ctx->chunks.empty())` 为 true → 触发 upload handler 路径 |
| `hash_ctx` | 已被 upload handler `EVP_MD_CTX_free()` 释放并置 nullptr | 第二个请求的 upload handler 访问 `nullptr` → 段错误 |
| `file_name` | 第一个请求的文件名 | 误用于第二个请求 |

`parser.reset()` 在 `http_parser.cpp:11` 中重置了 `chunk_handler_ = nullptr` 和 `headers_done_cb_ = nullptr`，因此第二个请求的 body 数据不再走流式 chunk_handler 回调，而是直接累积到 `request_.body` 中。但由于 `upload_ctx` 未重置，当第二个请求完成时，Handler 判断 `upload_ctx->chunks` 非空（来自第一个请求），尝试调用 upload handler → 访问已释放的 `hash_ctx` → 崩溃。

### 受影响的文件

| 文件 | 行号 | 问题 |
|------|------|------|
| `net/http/src/http_server.cpp` | `handle_connection():185` | `upload_ctx` 在循环外创建，循环内未重置 |
| `net/http/src/http_server.cpp` | `handle_connection():249` | `parser.reset()` 后继续循环，但 `upload_ctx` 残留 |
| `net/http/include/http_parser.h` | `reset():22-23` | 清除回调但不清除外部上下文 |

### 修复方案

**方案：** 每次请求完成后重新创建 `upload_ctx` 并重新注册 `headers_done_cb`，确保每个请求有独立的 `UploadStreamContext`。

**文件：** `net/http/src/http_server.cpp:185`

将 `upload_ctx` 创建和回调注册移到 `while` 循环**内部**，每次请求处理完成后更新：

```cpp
while (total_consumed < buf.size()) {
  auto upload_ctx = std::make_shared<UploadStreamContext>();
  parser.set_headers_done_callback(
    [this, &parser, upload_ctx](const HttpRequest& req) { on_headers_done(parser, req, upload_ctx); });

  // ... 解析、处理请求 ...

  // 请求完成时 parser.reset() 清除回调，下次循环创建新的 upload_ctx
}
```

移除外部的 `upload_ctx` 声明和 `set_headers_done_callback` 调用。
