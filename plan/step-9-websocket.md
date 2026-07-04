# Step 9：WebSocket 握手 + 帧编解码

> **状态**：已完成 ✅
> **优先级**：P1

## 背景

音乐播放需要实时通信（播放状态同步、歌单更新等），需实现 WebSocket 协议支持，包括 HTTP 升级握手和帧编解码。

## 功能点

| # | 功能点 | 优先级 | 说明 |
|---|--------|--------|------|
| F1 | **WebSocket 握手** | P0 | HTTP Upgrade 请求处理，Sec-WebSocket-Accept 计算 |
| F2 | **帧编解码** | P0 | 数据帧/控制帧的 mask/unmask、分片重组 |
| F3 | **WebSocket Server 集成** | P0 | 在 HttpServer handler 中检测 Upgrade 头并切换协议 |

## 实际实现

### 接口

```cpp
// websocket.h — 自由函数（无状态）
std::string base64_encode(const void* data, std::size_t len);
bool ws_server_handshake(const HttpRequest& req, HttpResponse& resp);
std::vector<char> ws_encode_frame(const void* data, std::size_t len, WsOpcode opcode);
std::optional<WsFrame> ws_decode_frame(std::string_view data);

// ws_connection.h — WsConnection 类
class WsConnection : public std::enable_shared_from_this<WsConnection> {
  void start_event_loop(ws_handler handler);  // 方案B：循环读帧
  void do_send(const std::vector<char>& frame);
  void do_close();
};
```

### 设计决策

| 决策 | 选择 |
|------|------|
| 编解码设计 | 自由函数式（无状态），帧分片由上层 WsConnection 管理 |
| 二进制存储 | `std::vector<char>`（非 `std::string`）|
| 同步保护 | per-connection `std::mutex`（`conn->write_mutex()`）|
| 心跳 | 不做，留给业务层 |
| HttpServer 集成 | `handle_connection` 检测 Upgrade 头 → `try_handle_ws_upgrade`（独立方法降低认知复杂度）|
| 循环依赖处理 | `http` 链接 `websocket`，`websocket` 仅 `add_includedirs(http)` 不链接 |
| flush 策略 | `do_send` + 升级握手后立即 `write_to_fd_locked()` 避免 event loop 阻塞时未发送 |

### 核心文件

| 文件 | 行数 | 职责 |
|------|------|------|
| `net/websocket/include/websocket.h` | 51 | WsOpcode/WsFrame 定义 + 函数声明 |
| `net/websocket/include/ws_connection.h` | 37 | WsConnection 类声明 |
| `net/websocket/src/websocket.cpp` | 98 | base64_encode / 握手 / 帧编解码实现 |
| `net/websocket/src/ws_connection.cpp` | 145 | WsConnection 事件循环 + 帧读写 |

### 修改的文件

| 文件 | 变更 |
|------|------|
| `net/http/include/i_http_server.h` | +ws() 方法, WsHandler 类型 |
| `net/http/include/http_server.h` | +try_handle_ws_upgrade, ws_handlers_ |
| `net/http/src/http_server.cpp` | +ws() 实现, Upgrade 检测, 101 flush |
| `net/http/xmake.lua` | +websocket include 路径 |
| `net/xmake.lua` | +includes("websocket") |
| `xmake.lua` | +websocket 测试依赖 |

### 测试用例

| # | 名称 | 说明 |
|---|------|------|
| T1 | ValidKey | 正常握手 key → Accept 计算 |
| T2 | MissingKey | 缺少 Sec-WebSocket-Key → 400 |
| T3 | MissingUpgrade | 缺少 Upgrade 头 → 400 |
| T4 | EncodeText | TEXT 帧编码（fin=1, opcode=1） |
| T5 | EncodeBinary | BINARY 帧编码（opcode=2） |
| T6 | EncodeClose | CLOSE 帧编码（opcode=8, payload=1000） |
| T7 | DecodeMasked | 客户端掩码帧解码 |
| T8 | DecodeUnmasked | 无掩码帧解码 |
| T9 | DecodeIncomplete | 不完整帧 → nullopt |
| T10 | DecodeFragmented | 分片帧重组（fin=0 + fin=1）|
| T11 | Base64Encode | Base64 编码向量（含 padding） |
| T12 | ServerHandshake | 端到端：连接 → 发送 Upgrade → 接收 101 → 帧收发 |

### 修复的 Bug

| Bug | 症状 | 修复 |
|-----|------|------|
| base64 padding 条件写反 | position 2/3 的剩余字节判断颠倒 | `pos==2→remaining>1`, `pos==3→remaining>2` |
| 101 响应未 flush | 集成测试 connect 后收不到 101 | `try_handle_ws_upgrade` 中追加 `write_to_fd_locked()` |

### 质量门禁

| 检查项 | 结果 |
|--------|------|
| clang-tidy | ✅ 0 error + 0 warning |
| cppcheck | ✅ 0 error + 0 warning + 0 style + 0 performance |
| CodeQL | ✅ 0 critical + 0 high |
| 编译 | ✅ 0 error + 0 warning |
| 测试 (12 WebSocket + 19 总用例) | ✅ 100% 通过 |
