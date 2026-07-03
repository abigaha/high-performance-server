# Step 9：WebSocket 握手 + 帧编解码

> **状态**：待开始
> **优先级**：P1

## 背景

音乐播放需要实时通信（播放状态同步、歌单更新等），需实现 WebSocket 协议支持，包括 HTTP 升级握手和帧编解码。

## 功能点

| # | 功能点 | 优先级 | 说明 |
|---|--------|--------|------|
| F1 | **WebSocket 握手** | P0 | HTTP Upgrade 请求处理，Sec-WebSocket-Accept 计算 |
| F2 | **帧编解码** | P0 | 数据帧/控制帧的 mask/unmask、分片重组 |
| F3 | **WebSocket Server 集成** | P0 | 在 HttpServer handler 中检测 Upgrade 头并切换协议 |

## 接口设计（初步）

```cpp
enum class WsOpcode : uint8_t {
  CONTINUATION = 0x0,
  TEXT = 0x1,
  BINARY = 0x2,
  CLOSE = 0x8,
  PING = 0x9,
  PONG = 0xA,
};

struct WsFrame {
  bool fin;
  WsOpcode opcode;
  bool masked;
  uint8_t mask_key[4];
  std::vector<char> payload;
};

// HTTP → WebSocket 升级
bool ws_handshake(const HttpRequest& req, HttpResponse& resp);

// 帧编码
std::vector<char> ws_encode(const void* data, std::size_t len, WsOpcode opcode, bool mask = false);

// 帧解码
std::optional<WsFrame> ws_decode(std::string_view data);
```

## 文件清单（预估）

| 操作 | 路径 |
|------|------|
| 创建 | `net/websocket/include/websocket.h` |
| 创建 | `net/websocket/src/websocket.cpp` |
| 创建 | `net/websocket/xmake.lua` |
| 创建 | `tests/test_websocket.cpp` |

## 测试用例（预估）

| # | 说明 |
|---|------|
| T1 | 握手请求生成与验证 |
| T2 | 帧编码正确（TEXT/BINARY/CLOSE）|
| T3 | 帧解码正确 |
| T4 | mask/unmask 正确 |
| T5 | 分片帧组装 |
