# Step 5：Range 流式传输

> **状态**：✅ 已完成（commit `ca6c157`）
> **起止**：file-system 完成后

## 背景

音乐播放需要进度控制（拖拽跳转），HTTP Range 请求（`Range: bytes=start-end`）是标准方案。在 file-system 模块基础上，扩展 HTTP 响应支持 `206 Partial Content`。

## 功能点

| # | 功能点 | 优先级 | 说明 |
|---|--------|--------|------|
| F1 | **Range 请求头解析** | P0 | `bytes=0-499` / `bytes=500-` / `bytes=-500` |
| F2 | **206 Partial Content 响应** | P0 | Content-Range 头 + 对应分块数据 |
| F3 | **多范围请求** | P1 | multipart/byteranges（未实现）|
| F4 | **流式传输** | P0 | 分块读取避免全量加载 |
| F5 | **路由集成** | P0 | handler 内检测 Range 头并响应 |

## 接口设计

```cpp
struct RangeInterval {
  std::size_t start;
  std::size_t end;
};

struct RangeRequest {
  std::vector<RangeInterval> ranges;
  bool valid{true};
  bool satisfiable{true};
};

RangeRequest parse_range_header(std::string_view header, std::size_t file_size);
void build_206_headers(HttpResponse& resp, const RangeRequest& range, std::size_t file_size);
void build_416_response(HttpResponse& resp, std::size_t file_size);
std::string generate_boundary();
```

## 文件清单

| 路径 | 说明 |
|------|------|
| `net/http/include/range_parser.h` | 解析器声明 |
| `net/http/src/range_parser.cpp` | 解析器实现 |

## 测试用例

| 测试文件 | 用例数 | 覆盖场景 |
|---------|--------|---------|
| `test_range_parser.cpp` | 10 | 单范围/开放/后缀/多范围/非法/416/206 headers/boundary |

## 质量门禁

| 检查项 | 结果 |
|--------|------|
| clang-tidy | ✅ 0 / 0 / 0 |
| cppcheck | ✅ 0 / 0 / 0 / 0 |
| 编译 | ✅ 0 error / 0 warning |
| 测试 | ✅ 10/10 通过 |
| CodeQL | ✅ 0 critical / 0 high |
