# Step 14：文件上传功能改进

> **状态**：分片上传、文件元数据、查询下载等主体能力已落地；上传类型与大小防护在后续前端优化中补强。
>
> 本文同时保留 Step 14 的设计背景并描述当前实现。早期 `verification/` 脚本已经删除，不能再作为现行验证入口。

## 原始问题与目标

早期 `POST /api/files/upload` 只按整体哈希保存文件，既不保留原文件名，也没有接入已有的分片能力。Step 14 的目标是：

- 保留并查询原文件名；
- 流式接收请求体并按 `2 MiB` 分片；
- 对分片和完整文件计算 SHA-256，以内容哈希去重；
- 在数据库中记录文件和分片关系；
- 提供列表、详情、下载、搜索和流式读取接口；
- 通过认证与角色限制控制访问和上传大小。

## 当前存储模型

```text
POST /api/files/upload + 原始文件请求体
  -> 从 Content-Disposition 解析文件名
  -> 在读取请求体前完成认证、类型、零字节和大小预检
  -> 边接收边切分为 2 MiB 分片
  -> 分片写入 data/chunks/<chunk_hash>
  -> 计算完整文件 SHA-256
  -> 写入 files 与 file_chunks
```

相同内容再次上传时按完整文件哈希命中已有记录，返回 `200` 和 `exists: true`；新文件创建成功返回 `201`。文件记录包含原文件名、完整哈希、大小、映射后的音频 Content-Type、分片大小和上传用户。

## 上传请求契约

前端直接以原始 `File` 作为请求体，不使用 `multipart/form-data`。请求至少需要：

```http
POST /api/files/upload HTTP/1.1
Authorization: Bearer <token>
Content-Disposition: attachment; filename="song.mp3"
Content-Length: <bytes>

<原始文件字节>
```

- 支持普通 `filename=`，也优先支持 RFC 5987 的 `filename*=UTF-8''...`，用于传递 UTF-8 文件名。
- 文件名会去除路径部分，避免把客户端路径当作服务端存储路径。
- CORS 允许浏览器发送 `Content-Disposition`，并向前端暴露下载响应中的该头。
- 缺少可解析的 `Content-Length` 时拒绝请求，避免在不知道请求体大小的情况下开始上传。

## 当前上传策略

### 扩展名白名单

后端只接受以下九种扩展名，匹配时不区分大小写：

| 扩展名 | 保存的 Content-Type |
|--------|---------------------|
| `.mp3` | `audio/mpeg` |
| `.ogg` | `audio/ogg` |
| `.wav` | `audio/wav` |
| `.flac` | `audio/flac` |
| `.aac` | `audio/aac` |
| `.m4a` | `audio/mp4` |
| `.wma` | `audio/x-ms-wma` |
| `.ape` | `audio/x-monkeys-audio` |
| `.opus` | `audio/opus` |

例如 `track.MP3` 和 `track.OpUs` 可以通过；无扩展名、`.txt`、`.bin` 或 `track.mp3.exe` 会在分片存储初始化前被拒绝。

### 认证、零字节和大小限制

| 条件 | 当前行为 |
|------|----------|
| 未登录或 GUEST | 不允许上传 |
| NORMAL | 默认最大 `10 MiB`，等于上限时允许 |
| VIP | 默认最大 `100 MiB`，等于上限时允许 |
| `Content-Length: 0` | 拒绝零字节文件 |
| 缺少有效文件名或 Content-Length | 拒绝请求 |

默认值来自 `ServerConfig`，当前 `config.json` 分别设置为 `10485760` 和 `104857600` 字节。部署入口 nginx 允许的请求体上限为 `110m`，但账号角色的最终上限由后端预检决定。

预检在分片目录写入、哈希上下文初始化和业务上传处理器之前执行。被拒绝的类型、零字节文件和超限文件不会进入分片落盘流程。

### 错误响应

上传策略返回结构化 JSON，前端应保留其中的详细后端错误：

| HTTP 状态 | `code` | 含义 |
|-----------|--------|------|
| `400` | `INVALID_FILE_NAME` | 缺少有效文件名 |
| `400` | `INVALID_CONTENT_LENGTH` | 缺少有效 Content-Length |
| `400` | `EMPTY_FILE` | 零字节文件 |
| `413` | `FILE_TOO_LARGE` | 超过当前角色上限，并返回实际大小和上限 |
| `415` | `UNSUPPORTED_FILE_TYPE` | 扩展名不在白名单，并返回允许的扩展名 |

## 重要安全边界

当前“文件类型校验”只检查文件名扩展名，并据此映射 Content-Type；不会检查音频魔数，也不会完整解码文件来确认其确实是可播放音频。因此：

- 白名单能拦截明显无关的文件名，但不能阻止攻击者把任意内容改成 `.mp3` 等允许后缀；
- 单文件大小限制能控制一次请求的最大体积，但不能替代总存储配额、恶意内容扫描和上传速率限制；
- 若业务需要把“可播放音频”作为强保证，应在后续引入经过验证的媒体探测或解码库，并在持久化前或隔离区内校验，不能把扩展名判断当作内容真实性证明。

前端也会做扩展名、MIME 冲突、零字节和角色大小预检，以便尽早提示用户；后端校验仍是不可绕过的最终边界。

## 相关接口

| 端点 | 作用 |
|------|------|
| `POST /api/files/upload` | 上传原始文件体，保留文件名并返回文件 ID、哈希、大小和分片数 |
| `GET /api/files` | 文件列表、名称和类型筛选、分页 |
| `GET /api/files/:id` | 按 ID 查询文件元数据 |
| `GET /api/files/:id/download` | 按 ID 下载并返回原文件名 |
| `GET /api/files/:id/stream` | 支持 Range 的音频读取 |
| `GET /api/files/by-hash/:hash/download` | 兼容按完整哈希下载 |

## 关键实现与测试

| 文件 | 职责 |
|------|------|
| `core/include/upload_policy.h`、`core/src/upload_policy.cpp` | 扩展名映射、角色大小策略和结构化错误 |
| `net/http/include/http_server.h`、`net/http/src/http_server.cpp` | 上传元数据解析、预检、流式分片和连接上下文 |
| `core/src/main.cpp` | 上传落库、去重、查询、下载及认证路由 |
| `db/schema.sql` 与数据库实现 | `files`、`file_chunks` 及相关读写 |
| `tests/test_upload_policy.cpp` | 九种扩展名、大小边界、零字节和落盘前拒绝 |
| `tests/test_http_server.cpp` | RFC 5987 文件名、路径清理、CORS 和上传连接行为 |
| `frontend/tests/` | 前端预检、错误保留和上传状态测试 |

## 现行验证入口

早期文档中的 `verification/verify.sh`、`verification/README.md` 和 `verification/ws_test.py` 已删除，只能作为历史记录理解，禁止继续引用为可执行入口。当前验证使用：

```bash
# 后端 Google Test 与前端 Vitest
bash scripts/test.sh

# 正式全量质量流水线
bash scripts/pipeline.sh all

# 部署后真实浏览器上传流程
bash scripts/docker.sh deploy
cd frontend
PLAYWRIGHT_BASE_URL=http://127.0.0.1:18080 npm run test:e2e
```

以上是执行方式，不代表本轮命令已经运行。具体通过情况必须以本次实际输出为准，不沿用 Step 14 早期的固定测试数量或旧质量门禁结论。
