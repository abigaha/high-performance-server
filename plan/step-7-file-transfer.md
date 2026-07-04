# Step 7：file-transfer 文件传输模块

> **状态**：✅ 已完成
> **优先级**：P0

## 背景

音乐软件需要传输音频文件。小文件单连接传输，大文件切割并行传输。需实现 `IFileTransfer` 抽象接口，以及 file-send-process / file-receive-process 独立进程。

## 功能点

| # | 功能点 | 优先级 | 说明 | 状态 |
|---|--------|--------|------|------|
| F1 | **IFileTransfer 抽象接口** | P0 | 文件传输操作抽象层 | ✅ |
| F2 | **小文件传输** | P0 | 单连接传输，依赖 ITcpClient | ✅ |
| F3 | **大文件传输** | P0 | fork/exec 唤起独立进程，切割并行传输 | ✅ |
| F4 | **file-send-process** | P0 | 发送端独立进程 | ✅ |
| F5 | **file-receive-process** | P0 | 接收端独立进程 | ✅ |

## 接口设计（初步）

```cpp
class IFileTransfer {
public:
  virtual ~IFileTransfer() = default;
  virtual bool transfer_small(const std::string& path, ITcpClient& client) = 0;
  virtual bool transfer_large(const std::string& path,
                              const std::string& peer_ip,
                              uint16_t peer_port) = 0;
  virtual bool receive_file(const std::string& save_path, ITcpServer& server) = 0;
};
```

## 文件清单（预估）

| 操作 | 路径 |
|------|------|
| 创建 | `net/file-transfer/include/i_file_transfer.h` |
| 创建 | `net/file-transfer/include/file_transfer.h` |
| 创建 | `net/file-transfer/src/file_transfer.cpp` |
| 创建 | `net/file-transfer/xmake.lua` |
| 创建 | `net/file-send-process/main.cpp` |
| 创建 | `net/file-receive-process/main.cpp` |
| 创建 | `tests/test_file_transfer.cpp` |

## 测试用例（预估）

| # | 说明 | 状态 |
|---|------|------|
| T1 | transfer_small 调用 ITcpClient 发送文件 | ✅ |
| T2 | transfer_small 文件不存在返回 false | ✅ |
| T3 | transfer_small 连接未建立返回 false | ✅ |
| T4 | ChunkHeader 网络字节序 roundtrip | ✅ |
| T5 | ChunkHeader 内存布局 28 字节 | ✅ |
| T6 | ChunkHeader 序列化/反序列化一致性 | ✅ |
| T7 | receive_file raw socket 接收单 chunk 并重组 | ✅ |
