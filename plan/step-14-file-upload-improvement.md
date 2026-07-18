# Step 14：文件上传功能改进

> **状态**：✅ 已完成
> **前置**：Bugfix 基准测试 Bug 修复完成后启动

## 问题

当前 `POST /api/files/upload` 两个核心缺陷：

| 缺陷 | 表现 |
|------|------|
| **不保留原文件名** | 上传内容存为 `uploads/<hash>`，`file_meta` 表无 `file_name`，无法按名查询 |
| **无分片存储** | 整个文件整体存为一个文件，`FileSystem::split_file()` / `compute_chunk_hash()` 存在但未使用，无法去重 |

导致：客户端只能通过预先知道的 hash 下载文件，无法搜索、无法列表、不可用。

## 方案

### 存储模型

```
请求: POST /api/files/upload  song.mp3 (20MB)
  → 流式分片: 边收边切，每分片 2MB
  → 计算每个 Chunk 的 SHA256
  → 存为 data/chunks/<chunk_hash>（hash 即文件名，天然去重）
  → DB 写两条记录:
       files:        (file_name="song.mp3", file_hash=<整体SHA256>, size, content_type)
       file_chunks:  (file_hash, chunk_index, chunk_hash, chunk_offset, chunk_size)
```

### DB schema 变更

```sql
CREATE TABLE IF NOT EXISTS files (
  file_id      BIGINT AUTO_INCREMENT PRIMARY KEY,
  file_name    VARCHAR(256) NOT NULL,
  file_hash    VARCHAR(64) NOT NULL,
  file_size    BIGINT NOT NULL DEFAULT 0,
  content_type VARCHAR(128),
  chunk_size   INT NOT NULL DEFAULT 2097152,
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_file_hash (file_hash),
  INDEX idx_file_name (file_name)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS file_chunks (
  file_hash    VARCHAR(64) NOT NULL,
  chunk_index  INT NOT NULL,
  chunk_hash   VARCHAR(64) NOT NULL,
  chunk_offset BIGINT NOT NULL,
  chunk_size   INT NOT NULL,
  PRIMARY KEY (file_hash, chunk_index),
  INDEX idx_chunk_hash (chunk_hash)
) ENGINE=InnoDB;
```

旧 `file_meta` 表保留做迁移过渡，新上传只写新表。

### API 变更

| 端点 | 变更 |
|------|------|
| `POST /api/files/upload` | 流式上传，保留原文件名（`Content-Disposition`），返回 `file_id` + `file_name` + `file_hash` |
| `GET /api/files` | 新增，列出所有文件（支持 `?name=` 模糊搜索 + 分页） |
| `GET /api/files/<id>` | 新增，按 ID 查询单个文件元信息 |
| `GET /api/files/<id>/download` | 新增，按 ID 下载 |
| `GET /api/files/by-hash/<hash>/download` | 保留，旧接口兼容 |

### 认证与权限

| 角色 | 权限 |
|------|------|
| GUEST | 无任何访问权限 |
| NORMAL | 上传 ≤10MB，文件浏览/下载 |
| VIP | 上传 ≤100MB，文件浏览/下载 |

Token 格式：`base64(payload).hmac_sha256_hex`，payload = `uid:role:exp`。

### 实现步骤

1. **DB 层**（`db/schema.sql` + `db/include/models.h` + `db/src/database_pool.cpp`）
   - 新增 `FileRecord` / `FileChunkRecord` 模型 + `AuthUser`
   - 新增 `store_file_record` / `get_file_record` / `search_files` / `store_file_chunks` / `get_file_chunks` / `chunk_exists` / `get_auth_user` / `verify_password`
   - 测试 +6 用例

2. **流式分片存储**（`net/http/src/http_server.cpp` + `core/src/main.cpp`）
   - HttpParser 流式模式：`kStreamChunkSize = 2097152`
   - `on_headers_done` 设置 chunk_handler，每 2MB 回调一次
   - 回调中：计算分片 SHA256 + `store_chunk_data(data, hash)` → `data/chunks/<chunk_hash>`
   - 最终 `EVP_DigestFinal_ex` 计算整体文件 SHA256
   - 双写 `files` + `file_chunks` 表

3. **查询/下载改造**（`core/src/main.cpp`）
   - `handle_download` 支持按 `file_id` / `file_name` / `file_hash` 查询
   - 从 `file_chunks` 逐块读取重组返回
   - 新增文件列表/搜索路由

4. **验证**（`verification/verify.sh` + 负载测试）
   - 上传带原名 → 响应含 `file_name`
   - 按名模糊搜索 → 命中
   - 按名下栽 → sha256sum 校验一致
   - 分片去重验证
   - 负载测试：7 级并发（1~1024），3 种载荷（1KB/1MB/5MB/10MB）

5. **Bug 修复**（详见 `bugfix-concurrent-uploads.md`）
   - Bug 1：`read_buffer_` 无锁跨线程访问 → malloc 堆损坏（`read_mutex_` 保护）
   - Bug 2：Keep-Alive 下 `upload_ctx` 复用污染（移至 while 循环内）

## 涉及文件

| 文件 | 变更 |
|------|------|
| `db/schema.sql` | 追加 `files` + `file_chunks` + `users` DDL |
| `db/include/models.h` | 新增 `FileRecord` / `FileChunkRecord` / `AuthUser` / `UserRole` |
| `db/include/idatabase_pool.h` | 新增 6 个纯虚方法 + 认证接口 |
| `db/include/database_pool.h` | override 声明 |
| `db/src/database_pool.cpp` | 实现新增方法 |
| `db/include/mock_connection.h` | 扩展 mock |
| `core/src/main.cpp` | upload/download/query 路由改造 + auth + RBAC |
| `net/http/include/http_server.h` | 新增 `UploadStreamContext` / `upload()` 方法 |
| `net/http/src/http_server.cpp` | 流式上传处理 (`on_headers_done`, `handle_connection`) |
| `net/http/include/http_parser.h` | 流式模式 + chunk_handler |
| `net/http/src/http_parser.cpp` | `feed_body_identity` 流式分片回调 |
| `net/tcp/tcp_server/include/connection.h` | 新增 `read_mutex_` |
| `net/tcp/tcp_server/include/connection.cpp` | `read_from_fd` / `consume_read_buffer` 加锁 |
| `net/websocket/src/ws_connection.cpp` | `read_buffer_` 加锁保护 |
| `net/http/include/auth_service.h` | 新增认证服务 |
| `net/http/include/auth_middleware.h` | RBAC Token 校验中间件 |
| `config.json` | 密码修正 + 数据库配置 |
| `tests/test_database_pool.cpp` | +6 用例 |
| `verification/verify.sh` | 新增分片上传验证 |
| `plan/development_plan.md` | 更新进度 |
| `plan/bugfix-concurrent-uploads.md` | 并发上传 Bug 修复文档 |
| `goal.md` | 更新续写进度 |

## 质量门禁

- [x] clang-tidy: 0 error + 0 warning + 0 style
- [x] cppcheck --enable=all: 0 error + 0 warning + 0 style + 0 performance
- [x] 编译: 0 error + 0 warning
- [x] xmake test: 100% 通过 (20 二进制)
- [x] CodeQL: 0 critical + 0 high severity
