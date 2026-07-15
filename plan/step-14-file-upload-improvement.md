# Step 14：文件上传功能改进

> **状态**：📋 待开始
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
  → 分片: Chunk 0..N 各 4MB
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
  chunk_size   INT NOT NULL DEFAULT 4194304,
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
| `POST /api/files/upload` | 支持原文件名（header/multipart），返回 `file_id` + `file_name` + `file_hash` |
| `GET /api/files` | 新增，列出所有文件（支持 `?name=` 模糊搜索 + 分页） |
| `GET /api/files/<id>` | 新增，按 ID 查询单个文件元信息 |
| `GET /api/files/<id>/download` | 新增，按 ID 下载 |
| `GET /api/files/by-hash/<hash>/download` | 保留，旧接口兼容 |

### 实现步骤

1. **DB 层**（`db/schema.sql` + `db/include/models.h` + `db/src/database_pool.cpp`）
   - 新增 `FileRecord` / `FileChunkRecord` 模型
   - 新增 `store_file_record` / `get_file_record` / `search_files` / `store_file_chunks` / `get_file_chunks` / `chunk_exists`
   - 测试 +6 用例

2. **分片存储改造**（`core/src/main.cpp` `handle_upload`）
   - 调用 `FileSystem::split_file()` 切割
   - 逐块 `compute_chunk_hash()` → 去重存储到 `chunks/<chunk_hash>`
   - 双写 `files` + `file_chunks` 表

3. **查询/下载改造**（`core/src/main.cpp`）
   - `handle_download` 支持按 `file_id` / `file_name` / `file_hash` 查询
   - 从 `file_chunks` 逐块读取重组返回
   - 新增文件列表/搜索路由

4. **验证**（`verification/verify.sh`）
   - 上传带原名 → 响应含 `file_name`
   - 按名模糊搜索 → 命中
   - 按名下栽 → sha256sum 校验一致
   - 分片去重验证

## 涉及文件

| 文件 | 变更 |
|------|------|
| `db/schema.sql` | 追加 `files` + `file_chunks` DDL |
| `db/include/models.h` | 新增 `FileRecord` / `FileChunkRecord` |
| `db/include/idatabase_pool.h` | 新增 6 个纯虚方法 |
| `db/include/database_pool.h` | override 声明 |
| `db/src/database_pool.cpp` | 实现新增方法 |
| `db/include/mock_connection.h` | 扩展 mock |
| `core/src/main.cpp` | upload/download/query 路由改造 |
| `tests/test_database_pool.cpp` | +6 用例 |
| `verification/verify.sh` | 新增分片上传验证 |
| `plan/development_plan.md` | 更新进度 |
| `goal.md` | 更新续写进度 |

## 质量门禁

- [ ] clang-tidy: 0 error + 0 warning + 0 style
- [ ] cppcheck --enable=all: 0 error + 0 warning + 0 style + 0 performance
- [ ] 编译: 0 error + 0 warning
- [ ] xmake test: 100% 通过
- [ ] CodeQL: 0 critical + 0 high severity
