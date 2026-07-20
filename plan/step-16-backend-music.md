# Step 16 — 后端数据库重构 + 音乐库/歌单 API

> 数据库范式优化、AST 查询优化、新增音乐库（Music Library）+ 用户歌单（Playlist）系统、补齐缺失 API。

---

## 一、概览

### 目标

1. 数据库表结构**范式化重构**（3NF），将 `file_records` 中的音乐元信息拆分到 `music_meta` 独立表
2. **代数语法树（AST）优化**：索引设计、谓词下推、查询重写，保证歌单查询性能
3. 新增**音乐库 + 用户歌单**完整子系统
4. 补齐前端所需的全部缺失 API（认证/文件/用户）

### 变更范围

| 模块 | 文件 | 变更类型 |
|------|------|---------|
| `db/include/models.h` | 数据模型 | 新增 MusicMeta, Playlist, PlaylistItem 结构体 |
| `db/include/idatabase_pool.h` | 数据库接口 | 新增音乐/歌单相关纯虚方法 |
| `db/include/database_pool.h` | 数据库实现 | 新增音乐/歌单方法实现 |
| `db/src/database_pool.cpp` | 数据库实现 | 实现 SQL 查询（含 AST 优化） |
| `db/src/boost_mysql_connection.cpp` | SQL 连接 | （如需要扩展 Schema 初始化） |
| `core/src/main.cpp` | 路由注册 | 新增 N1–N11 + M1–M8 路由 |
| `net/http/include/http_server.h` | HTTP 服务器 | 无需改动（已有 get/post/put/del） |
| `net/http/src/http_server.cpp` | HTTP 服务器 | CORS 头全局注入 + OPTIONS 204 处理 |
| `net/http/src/range_parser.cpp` | Range 解析 | 无需改动（现有实现完整） |
| `data/schema.sql` | DDL | 全量 DDL 更新 |

---

## 二、数据库范式优化（3NF）

### 2.1 现有表结构问题

当前 `file_records` 表将所有文件元信息揉在一起：

```
file_records (当前)
├── file_id, file_name, file_hash, file_size, content_type, chunk_size, created_at
├── 没有音乐专用字段（artist, album, genre, duration）
└── 没有关联到音乐元信息
```

问题：
- 文件存储与音乐元信息**紧耦合**，一个文件一条记录无法表达多文件同歌曲（不同音质）
- 没有艺术家/专辑/流派独立维度，无法高效按流派查询
- 音乐文件的哈希、格式等存储属性与业务属性混在同一表

### 2.2 目标范式设计（3NF）

```
users (已存在，扩展 email/role)
  │
  ├──< user_id
  │
  ├─── user_playlists          (1 用户 → N 歌单)
  │     ├── playlist_id (PK)
  │     ├── user_id (FK → users)
  │     ├── name
  │     ├── description
  │     └── created_at
  │           │
  │           └──< playlist_id
  │                 │
  │                 └─── playlist_items      (N 歌单 ⇄ N 音乐)
  │                       ├── id (PK)
  │                       ├── playlist_id (FK → user_playlists)
  │                       ├── music_id (FK → music_meta)
  │                       ├── sort_order
  │                       └── added_at
  │
  └─── file_records (范式化后)  (1 音乐 → N 文件版本)
        ├── file_id (PK)
        ├── music_id (FK → music_meta)
        ├── file_name
        ├── file_hash (UNIQUE)
        ├── file_size
        ├── content_type
        ├── uploaded_by (FK → users)
        ├── chunk_size
        └── created_at

music_meta (新增——音乐核心元信息)
  ├── music_id (PK)
  ├── title          (歌曲名)
  ├── artist         (艺术家)
  ├── album          (专辑)
  ├── genre          (流派)
  ├── duration_sec   (时长，秒)
  ├── track_number   (曲目号)
  ├── created_at
  └── updated_at
```

### 2.3 范式化收益

| 维度 | 之前 | 之后 |
|------|------|------|
| 冗余度 | 同一歌曲多种格式需重复 title/artist | 只存一次在 music_meta，file_records 关联 music_id |
| 查询复杂度 | `LIKE '%artist%'` 扫全表 | `WHERE artist = ?` 走索引 |
| 扩展性 | 加 metadata 字段需改表 | 独立表不影响 file_records |
| 多版本文件 | 不原生支持 | 一个 music_id 关联多个 file (FLAC/MP3/OGG) |

---

## 三、数据库 AST 查询优化

> 代数语法树（Algebraic Syntax Tree）优化 = **选择合适的索引 + 重写查询谓词顺序 + 减少扫描行数**

### 3.1 核心索引设计

```sql
-- music_meta 查询索引
CREATE INDEX idx_music_title   ON music_meta(title(64));     -- 前缀索引，支持 LIKE 'xxx%'
CREATE INDEX idx_music_artist  ON music_meta(artist(64));    -- 艺术家精确查
CREATE INDEX idx_music_album   ON music_meta(album(64));     -- 专辑精确查
CREATE INDEX idx_music_genre   ON music_meta(genre(32));     -- 流派过滤

-- file_records 查询索引
CREATE INDEX idx_file_music     ON file_records(music_id);
CREATE INDEX idx_file_hash      ON file_records(file_hash);
CREATE INDEX idx_file_type      ON file_records(content_type(32));
CREATE INDEX idx_file_uploader  ON file_records(uploaded_by);

-- playlist_items 查询索引
CREATE INDEX idx_playlist_item_lookup ON playlist_items(playlist_id, sort_order, music_id);

-- 用户的歌单查询
CREATE INDEX idx_user_playlist  ON user_playlists(user_id, created_at DESC);
```

### 3.2 关键查询 AST 优化

#### 查询 1：用户歌单 + 音乐详情（JOIN 顺序优化）

```sql
-- 优化前（笛卡尔积风险）:
SELECT * FROM playlist_items pi
JOIN music_meta m ON pi.music_id = m.music_id
JOIN file_records f ON f.music_id = m.music_id
WHERE pi.playlist_id = ?
ORDER BY pi.sort_order;

-- 优化后（先过滤 playlist_id 缩小 result set，再 JOIN）:
SELECT m.music_id, m.title, m.artist, m.album, m.duration_sec,
       f.file_id, f.file_hash, f.file_size, f.content_type
FROM playlist_items pi
STRAIGHT_JOIN music_meta m ON m.music_id = pi.music_id
LEFT JOIN file_records f ON f.music_id = m.music_id AND f.content_type LIKE 'audio/%'
WHERE pi.playlist_id = ?
ORDER BY pi.sort_order;
```

**AST 优化点：**
- `STRAIGHT_JOIN` 强制驱动表为 `playlist_items`（通过 `playlist_id` 索引极速定位）
- `WHERE pi.playlist_id = ?` 谓词下推至最内层，先过滤再 JOIN
- `file_records` 的 `content_type LIKE 'audio/%'` 谓词在 JOIN 时即过滤，避免查出非音频文件
- 覆盖索引：`idx_playlist_item_lookup(playlist_id, sort_order, music_id)` 提供索引覆盖，无需回表

#### 查询 2：全局音乐库搜索（谓词下推 + 索引跳扫）

```sql
-- 按标题搜索（前缀匹配，索引跳扫）:
SELECT m.*, f.file_hash, f.file_size, f.content_type
FROM music_meta m
LEFT JOIN file_records f ON f.music_id = m.music_id
WHERE m.title LIKE CONCAT(?, '%')
ORDER BY m.title
LIMIT ? OFFSET ?;
```

**AST 优化点：**
- `m.title LIKE 'xxx%'` 走 `idx_music_title` 索引范围扫描，不走全表
- `LIMIT ? OFFSET ?` 配合索引排序避免 filesort

#### 查询 3：文件列表按类型过滤

```sql
-- 按 content_type 过滤（类型索引 + 延迟 JOIN）:
SELECT f.file_id, f.file_name, f.file_hash, f.file_size, f.content_type,
       m.music_id, m.title, m.artist
FROM file_records f
LEFT JOIN music_meta m ON m.music_id = f.music_id
WHERE f.content_type LIKE CONCAT(?, '%')
ORDER BY f.created_at DESC
LIMIT ? OFFSET ?;
```

**AST 优化点：**
- `f.content_type LIKE 'audio/%'` 走 `idx_file_type` 索引
- `LEFT JOIN music_meta` 只对有关联的行做 JOIN，普通文件 `music_id = NULL` 不影响性能

### 3.3 慢查询监控

```sql
-- 启用慢查询日志（MySQL 配置）
SET GLOBAL slow_query_log = ON;
SET GLOBAL long_query_time = 0.5;          -- 500ms 以上记录
SET GLOBAL log_queries_not_using_indexes = ON;
```

---

## 四、数据模型（C++ 结构体）

```cpp
// db/include/models.h (新增)

struct MusicMeta {
  int64_t music_id{0};
  std::string title;
  std::string artist;
  std::string album;
  std::string genre;
  int duration_sec{0};
  int track_number{0};
  std::string created_at;
  std::string updated_at;
};

struct Playlist {
  int64_t playlist_id{0};
  int64_t user_id{0};
  std::string name;
  std::string description;
  int item_count{0};
  std::string created_at;
};

struct PlaylistItem {
  int64_t id{0};
  int64_t playlist_id{0};
  int64_t music_id{0};
  std::string title;     // 冗余便于展示
  std::string artist;
  std::string file_hash;
  int sort_order{0};
  std::string added_at;
};

// 扩展 FileRecord（music_id 字段）
struct FileRecord {
  int64_t file_id{0};
  int64_t music_id{0};
  std::string file_name;
  std::string file_hash;
  std::size_t file_size{0};
  std::string content_type;
  int chunk_size{2097152};
  int64_t uploaded_by{0};
  std::string created_at;
};
```

---

## 五、数据库接口扩展（IDatabasePool）

```cpp
// db/include/idatabase_pool.h (新增纯虚方法)

class IDatabasePool {
public:
  // === 已有方法 ===
  virtual bool init(const DbConfig& config) = 0;
  virtual void close() = 0;
  virtual std::optional<User> get_user(int64_t user_id) = 0;
  virtual bool create_user(const User& user) = 0;
  virtual bool store_file_record(const FileRecord& record) = 0;
  virtual std::optional<FileRecord> get_file_record(int64_t file_id) = 0;
  virtual std::optional<FileRecord> get_file_record_by_hash(const std::string& hash) = 0;
  virtual std::vector<FileRecord> search_files(const std::string& name_pattern,
                                                int offset, int limit) = 0;
  virtual bool store_file_chunks(const std::vector<FileChunkRecord>& chunks) = 0;
  virtual std::vector<FileChunkRecord> get_file_chunks(const std::string& file_hash) = 0;
  virtual bool chunk_exists(const std::string& chunk_hash) = 0;
  virtual std::optional<AuthUser> get_auth_user(const std::string& username) = 0;
  virtual bool verify_password(const std::string& username,
                                const std::string& password) = 0;

  // === 新增：认证 ===
  virtual std::optional<AuthUser> get_user_by_token(const std::string& token,
                                                      IAuthService& auth) = 0;
  virtual bool username_exists(const std::string& username) = 0;

  // === 新增：文件扩展 ===
  virtual std::vector<FileRecord> search_files_ext(const std::string& name_pattern,
                                                    const std::string& type_filter,
                                                    int offset, int limit,
                                                    int& out_total) = 0;
  virtual bool delete_file_record(int64_t file_id) = 0;
  virtual bool update_file_record(const FileRecord& record) = 0;
  virtual bool update_user(const User& user) = 0;

  // === 新增：音乐库 ===
  virtual std::vector<MusicMeta> list_music_library(const std::string& search,
                                                     int offset, int limit,
                                                     int& out_total) = 0;
  virtual std::optional<MusicMeta> get_music_meta(int64_t music_id) = 0;
  virtual std::optional<MusicMeta> get_music_by_file_id(int64_t file_id) = 0;
  virtual int64_t create_music_meta(const MusicMeta& meta) = 0;
  virtual bool update_music_meta(const MusicMeta& meta) = 0;
  virtual bool delete_music_meta(int64_t music_id) = 0;

  // === 新增：歌单 ===
  virtual std::vector<Playlist> get_user_playlists(int64_t user_id) = 0;
  virtual int64_t create_playlist(const Playlist& pl) = 0;
  virtual bool delete_playlist(int64_t playlist_id) = 0;
  virtual std::vector<PlaylistItem> get_playlist_items(int64_t playlist_id) = 0;
  virtual bool add_playlist_item(int64_t playlist_id, int64_t music_id) = 0;
  virtual bool remove_playlist_item(int64_t playlist_id, int64_t music_id) = 0;
  virtual bool reorder_playlist_items(int64_t playlist_id,
                                       const std::vector<int64_t>& music_ids) = 0;
};
```

---

## 六、DDL（完整建表 SQL）

```sql
-- ========== 用户表 ==========
CREATE TABLE IF NOT EXISTS users (
  user_id      BIGINT AUTO_INCREMENT PRIMARY KEY,
  username     VARCHAR(64) NOT NULL UNIQUE,
  password_hash VARCHAR(256) NOT NULL,
  email        VARCHAR(128) DEFAULT '',
  role         TINYINT NOT NULL DEFAULT 1,   -- 0=GUEST, 1=NORMAL, 2=VIP
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_users_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ========== 音乐元信息表 ==========
CREATE TABLE IF NOT EXISTS music_meta (
  music_id     BIGINT AUTO_INCREMENT PRIMARY KEY,
  title        VARCHAR(256) NOT NULL,
  artist       VARCHAR(256) NOT NULL DEFAULT '',
  album        VARCHAR(256) NOT NULL DEFAULT '',
  genre        VARCHAR(64)  NOT NULL DEFAULT '',
  duration_sec INT NOT NULL DEFAULT 0,
  track_number INT NOT NULL DEFAULT 0,
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  updated_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  INDEX idx_music_title  (title(64)),
  INDEX idx_music_artist (artist(64)),
  INDEX idx_music_album  (album(64)),
  INDEX idx_music_genre  (genre(32))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ========== 文件记录表（范式化后，关联 music_meta）==========
CREATE TABLE IF NOT EXISTS file_records (
  file_id      BIGINT AUTO_INCREMENT PRIMARY KEY,
  music_id     BIGINT DEFAULT NULL,
  file_name    VARCHAR(256) NOT NULL,
  file_hash    VARCHAR(64) NOT NULL UNIQUE,
  file_size    BIGINT NOT NULL DEFAULT 0,
  content_type VARCHAR(64) NOT NULL DEFAULT 'application/octet-stream',
  chunk_size   INT NOT NULL DEFAULT 2097152,
  uploaded_by  BIGINT NOT NULL DEFAULT 0,
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_file_music    (music_id),
  INDEX idx_file_hash     (file_hash),
  INDEX idx_file_type     (content_type(32)),
  INDEX idx_file_uploader (uploaded_by),
  FOREIGN KEY (music_id) REFERENCES music_meta(music_id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ========== 文件分片记录表 ==========
CREATE TABLE IF NOT EXISTS file_chunks (
  chunk_id     BIGINT AUTO_INCREMENT PRIMARY KEY,
  file_hash    VARCHAR(64) NOT NULL,
  chunk_index  INT NOT NULL,
  chunk_hash   VARCHAR(64) NOT NULL,
  chunk_offset BIGINT NOT NULL DEFAULT 0,
  chunk_size   INT NOT NULL DEFAULT 0,
  INDEX idx_chunks_file_hash (file_hash),
  FOREIGN KEY (file_hash) REFERENCES file_records(file_hash) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ========== 用户歌单表 ==========
CREATE TABLE IF NOT EXISTS user_playlists (
  playlist_id  BIGINT AUTO_INCREMENT PRIMARY KEY,
  user_id      BIGINT NOT NULL,
  name         VARCHAR(128) NOT NULL DEFAULT '默认歌单',
  description  VARCHAR(512) NOT NULL DEFAULT '',
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_user_playlist (user_id, created_at DESC),
  FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ========== 歌单项表 ==========
CREATE TABLE IF NOT EXISTS playlist_items (
  id           BIGINT AUTO_INCREMENT PRIMARY KEY,
  playlist_id  BIGINT NOT NULL,
  music_id     BIGINT NOT NULL,
  sort_order   INT NOT NULL DEFAULT 0,
  added_at     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_playlist_music (playlist_id, music_id),
  INDEX idx_playlist_item_lookup (playlist_id, sort_order, music_id),
  FOREIGN KEY (playlist_id) REFERENCES user_playlists(playlist_id) ON DELETE CASCADE,
  FOREIGN KEY (music_id) REFERENCES music_meta(music_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

---

## 七、API 端点设计（完整清单）

### 7.1 认证（N1/N5/N8/N11）

#### N1 — POST /api/auth/register

```
Request:  { "username": "alice", "password": "123456", "email": "a@b.com" }
Response 201:
{
  "token": "dWlk...c2ln",
  "user_id": 1,
  "username": "alice",
  "role": 1
}
Error 400: { "error": "用户名已存在" }
Error 400: { "error": "用户名或密码不能为空" }

逻辑:
  1. 校验 username(≥2字符) + password(≥6字符)
  2. 检查 username 是否已存在（db.username_exists）
  3. bcrypt/加盐哈希 password（使用 OpenSSL EVP 或 libbcrypt）
  4. db.create_user({ username, password_hash, email, role: NORMAL })
  5. auth.generate_token(user) → 返回 token
```

#### N11 — POST /api/auth/logout

```
Request:  Authorization: Bearer <token>
Response 200: { "message": "已登出" }

逻辑:
  - 当前实现 Token 无服务端状态，可仅返回成功
  - 未来可扩展 Token 黑名单
```

#### N8 — GET /api/auth/me

```
Request:  Authorization: Bearer <token>
Response 200:
{
  "user_id": 1,
  "username": "alice",
  "email": "a@b.com",
  "role": "NORMAL",
  "created_at": "2024-01-01 00:00:00"
}
Error 401: { "error": "未登录" }

逻辑:
  1. AuthMiddleware 已解析 req.auth_user
  2. db.get_user(req.auth_user.user_id) → 返回完整信息
```

### 7.2 文件（N3/N4/N7/N9/N10）

#### N7+N9 — GET /api/files?name=&type=&offset=&limit=

```
Request:  GET /api/files?name=abc&type=audio&offset=0&limit=20
Response 200:
{
  "items": [
    {
      "file_id": 1,
      "file_name": "song.mp3",
      "file_hash": "abc123",
      "file_size": 5242880,
      "content_type": "audio/mpeg",
      "music_id": 1,
      "title": "Song Title",
      "artist": "Artist Name"
    }
  ],
  "total": 42,
  "offset": 0,
  "limit": 20
}

逻辑:
  1. 调用 db.search_files_ext(name, type, offset, limit, out_total)
  2. type 参数对应 content_type LIKE 'audio/%' 或 'application/%'
  3. 结果包含 total 总数
```

#### N4 — GET /api/files/search?q=&sort=&offset=&limit=

```
Request:  GET /api/files/search?q=abc&sort=size_desc&offset=0&limit=20
Response 200:
{
  "items": [...],
  "total": 10,
  "offset": 0,
  "limit": 20
}

逻辑:
  1. q 搜索文件名 + 音乐标题
  2. sort: name_asc / name_desc / size_asc / size_desc / date_asc / date_desc
```

#### N3 — GET /api/files/:id/stream（音频流播放）

```
Request:  GET /api/files/:id/stream
          Range: bytes=0-4095           (可选，支持拖拽 seek)
Response 200 (无 Range):
  Content-Type: audio/mpeg
  Accept-Ranges: bytes
  Content-Length: <全量大小>
  <完整音频二进制>

Response 206 (有 Range):
  Status: 206 Partial Content
  Content-Type: audio/mpeg
  Content-Range: bytes 0-4095/5242880
  Content-Length: 4096
  Accept-Ranges: bytes
  <范围二进制>

Error 404: { "error": "file not found" }
Error 416: { "error": "Range Not Satisfiable" }

逻辑:
  1. 查 file_records + music_meta
  2. 检测 content_type（根据扩展名或 DB 存储值）
  3. 如果请求头有 Range:
     a. parse_range_header(req.headers["Range"], file_size)
     b. 只读取范围内的 chunk 数据（分块读取，避免全量加载到内存）
     c. 返回 206 + Content-Range
  4. 如果无 Range: 返回 200 + 完整内容（但不设 Content-Disposition: attachment）
  5. 响应头加 Accept-Ranges: bytes
```

#### N10 — DELETE /api/files/:id

```
Request:  DELETE /api/files/:id
          Authorization: Bearer <token> (VIP only)
Response 200: { "message": "已删除" }
Error 403: { "error": "权限不足" }
Error 404: { "error": "file not found" }

逻辑:
  1. check_auth(req, resp, VIP)
  2. 查 file_record
  3. 删除文件分片（文件系统 + DB）
  4. 删除 file_record（CASCADE 删除 chunks）
  5. 如果关联 music_id 且无其他 file → 删除 music_meta
```

### 7.3 用户（N2）

#### N2 — PUT /api/users/:id

```
Request:  PUT /api/users/:id
          Authorization: Bearer <token>
          { "email": "new@b.com", "password": "newpass123" }
Response 200: { "message": "已更新" }
Error 403: { "error": "只能修改自己的信息" }
Error 400: { "error": "邮箱格式错误" }

逻辑:
  1. 验证 req.auth_user.user_id == :id（只能修改自己的）
  2. 校验 email 格式（如有）
  3. 如果 password 不为空 → 重新哈希
  4. db.update_user(...)
```

### 7.4 音乐库（M1–M2）

#### M1 — GET /api/music/library?search=&offset=&limit=

```
Request:  GET /api/music/library?search=love&offset=0&limit=20
Response 200:
{
  "items": [
    {
      "music_id": 1,
      "title": "Love Story",
      "artist": "Artist",
      "album": "Album",
      "genre": "Pop",
      "duration_sec": 240,
      "file_hash": "abc123",
      "file_size": 5242880,
      "content_type": "audio/mpeg"
    }
  ],
  "total": 100,
  "offset": 0,
  "limit": 20
}

逻辑:
  1. db.list_music_library(search, offset, limit, out_total)
  2. search 在 title / artist / album 中 LIKE 匹配
  3. 只返回有关联 file_records 的音乐（即有实际文件）
```

#### M2 — GET /api/music/library/:id

```
Request:  GET /api/music/library/1
Response 200:
{
  "music_id": 1,
  "title": "Love Story",
  "artist": "Artist",
  "album": "Album",
  "genre": "Pop",
  "duration_sec": 240,
  "files": [
    { "file_id": 1, "file_hash": "abc", "file_size": 5242880, "content_type": "audio/mpeg" },
    { "file_id": 2, "file_hash": "def", "file_size": 1048576, "content_type": "audio/ogg" }
  ]
}

逻辑:
  1. db.get_music_meta(id) + db.get_file_records_by_music(id)
  2. 返回音乐详情 + 所有可用文件版本
```

### 7.5 歌单（M3–M8）

#### M3 — GET /api/users/:id/playlists

```
Request:  GET /api/users/1/playlists
Response 200:
{
  "playlists": [
    { "id": 1, "name": "我的最爱", "description": "", "item_count": 5, "created_at": "..." },
    { "id": 2, "name": "跑步歌单", "description": "运动时听", "item_count": 3, "created_at": "..." }
  ]
}
```

#### M4 — POST /api/users/:id/playlists

```
Request:  POST /api/users/1/playlists
          { "name": "新歌单", "description": "描述" }
Response 201: { "playlist_id": 3, "name": "新歌单" }
```

#### M5 — GET /api/playlists/:id/items

```
Request:  GET /api/playlists/1/items
Response 200:
{
  "playlist_id": 1,
  "name": "我的最爱",
  "items": [
    {
      "id": 1,
      "music_id": 5,
      "title": "Love Story",
      "artist": "Artist",
      "file_hash": "abc123",
      "sort_order": 0,
      "added_at": "..."
    }
  ]
}
```

#### M6 — POST /api/playlists/:id/items

```
Request:  POST /api/playlists/1/items
          { "music_id": 5 }
Response 201: { "message": "已添加" }
Error 409: { "error": "歌曲已在歌单中" }
```

#### M7 — DELETE /api/playlists/:id/items/:music_id

```
Request:  DELETE /api/playlists/1/items/5
Response 200: { "message": "已移除" }
```

#### M8 — PUT /api/playlists/:id/items/reorder

```
Request:  PUT /api/playlists/1/items/reorder
          { "music_ids": [3, 1, 2] }
Response 200: { "message": "排序已更新" }
```

---

## 八、CORS 实现（N5）

```cpp
// net/http/src/http_server.cpp — 修改 handle_connection

void HttpServer::handle_connection(Connection& conn) {
  // ... 现有代码 ...

  // 处理 OPTIONS 预检请求
  if (parser.method() == HttpMethod::OPTIONS) {
    HttpResponse resp;
    resp.set_status(204, "No Content");
    add_cors_headers(resp);
    conn.send_response(resp);
    return;
  }

  // 所有响应注入 CORS 头
  // 在 HttpResponse::send() 或 handler 调用后追加
}

// 公共 CORS 头注入
static void add_cors_headers(HttpResponse& resp) {
  resp.set_header("Access-Control-Allow-Origin", "*");
  resp.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  resp.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type, Range");
  resp.set_header("Access-Control-Expose-Headers", "Content-Range, Accept-Ranges, Content-Disposition");
  resp.set_header("Access-Control-Max-Age", "86400");
}
```

---

## 九、上传时 Content-Type 自动检测（N9）

```cpp
// core/src/main.cpp — upload_setup 中扩展

static std::string detect_content_type(const std::string& filename) {
  auto dot = filename.rfind('.');
  if (dot == std::string::npos) return "application/octet-stream";
  auto ext = filename.substr(dot);
  if (ext == ".mp3")  return "audio/mpeg";
  if (ext == ".ogg")  return "audio/ogg";
  if (ext == ".wav")  return "audio/wav";
  if (ext == ".flac") return "audio/flac";
  if (ext == ".aac")  return "audio/aac";
  if (ext == ".m4a")  return "audio/mp4";
  if (ext == ".wma")  return "audio/x-ms-wma";
  if (ext == ".ape")  return "audio/x-monkeys-audio";
  if (ext == ".opus") return "audio/opus";
  return "application/octet-stream";
}

// 上传完成后或 parser 解析时，根据文件名检测 content_type → 写入 FileRecord
// 如果文件是音频类型（audio/*），自动创建 music_meta 记录
```

---

## 十、音频流播实现（N3 详细设计）

### 10.1 分块读取（避免全量加载到内存）

```cpp
server.get("/api/files/:id/stream", [&db, &fs](const HttpRequest& req, HttpResponse& resp) {
  // 1. 查文件 + 音乐信息
  auto it = req.path_params.find("id");
  auto record = db.get_file_record(std::stoll(it->second));
  if (!record) { /* 404 */ return; }

  // 2. 检测 content_type
  auto content_type = record->content_type;
  if (content_type == "application/octet-stream") {
    content_type = detect_content_type(record->file_name);
  }

  // 3. 获取文件分片
  auto chunks = db.get_file_chunks(record->file_hash);

  // 4. 获取全量文件大小
  std::size_t file_size = record->file_size;

  // 5. 设置公共头
  resp.set_header("Accept-Ranges", "bytes");
  resp.set_content_type(content_type);

  // 6. 解析 Range
  auto range_it = req.headers.find("Range");
  if (range_it != req.headers.end()) {
    auto range_req = parse_range_header(range_it->second, file_size);
    if (!range_req.valid || !range_req.satisfiable || range_req.ranges.empty()) {
      build_416_response(resp, file_size);
      return;
    }
    // 单 Range 查询（音频 seek 场景）
    const auto& iv = range_req.ranges[0];
    build_206_headers(resp, range_req, file_size);
    resp.set_content_type(content_type);

    // 只读取范围内的数据
    std::string body_data;
    body_data.reserve(iv.end - iv.start);
    std::size_t remain = iv.end - iv.start;
    std::size_t offset = iv.start;
    for (const auto& c : chunks) {
      if (remain == 0) break;
      if (offset >= static_cast<std::size_t>(c.chunk_size)) {
        offset -= c.chunk_size;
        continue;
      }
      auto data = fs.read_file("chunks/" + c.chunk_hash);
      if (!data) { /* 500 */ return; }
      auto start = offset;
      auto count = std::min<std::size_t>(data->size() - start, remain);
      body_data.append(data->data() + start, count);
      remain -= count;
      offset = 0;
    }
    resp.body = std::move(body_data);
    resp.set_content_length(iv.end - iv.start);
  } else {
    // 无 Range → 返回完整文件（不设 Content-Disposition）
    resp.set_status(200, "OK");
    for (const auto& c : chunks) {
      auto data = fs.read_file("chunks/" + c.chunk_hash);
      if (!data) { /* 500 */ return; }
      resp.body.append(data->data(), data->size());
    }
    resp.set_content_length(file_size);
  }
});
```

---

## 十一、实现顺序（Phases）

### Phase A — 基础设施 + 认证（P0）

| 任务 | 涉及文件 | 说明 |
|------|---------|------|
| DDL 更新 | `data/schema.sql` | 新建表、字段 |
| CORS | `http_server.cpp` | 全局 CORS 头 + OPTIONS 204 |
| N1 注册 | `main.cpp` | POST /api/auth/register |
| N8 /me | `main.cpp` | GET /api/auth/me |
| N11 登出 | `main.cpp` | POST /api/auth/logout |

### Phase B — 文件系统增强（P0–P1）

| 任务 | 涉及文件 | 说明 |
|------|---------|------|
| N3 音频流 | `main.cpp` | GET /api/files/:id/stream + Range |
| N6 Accept-Ranges | `main.cpp` | 所有文件响应加 Accept-Ranges |
| N7+N9 文件扩展 | `main.cpp` + `database_pool.cpp` | type 过滤 + total 总数 + content_type 检测 |
| N4 搜索增强 | `main.cpp` | GET /api/files/search |

### Phase C — 音乐库 + 歌单（P1）

| 任务 | 涉及文件 | 说明 |
|------|---------|------|
| 数据模型 | `models.h` | MusicMeta, Playlist, PlaylistItem |
| DB 接口 | `idatabase_pool.h` + `database_pool.h/cpp` | 新增全部纯虚方法 + 实现 |
| M1–M2 音乐库 | `main.cpp` | GET /api/music/library + /:id |
| M3–M8 歌单 | `main.cpp` | 歌单 CRUD + 排序 |

### Phase D — 用户管理 + 删除（P1–P2）

| 任务 | 涉及文件 | 说明 |
|------|---------|------|
| N2 用户编辑 | `main.cpp` | PUT /api/users/:id |
| N10 文件删除 | `main.cpp` | DELETE /api/files/:id |
| 上传自动创建 music_meta | `main.cpp` upload handler | 检测 audio/* → 自动创建 music_meta |

---

## 十二、质量门禁

| 门禁 | 标准 |
|------|------|
| clang-tidy | 0 error + 0 warning + 0 style |
| cppcheck --enable=all | 0 error + 0 warning + 0 style + 0 performance |
| CodeQL | 0 critical + 0 high |
| 编译 | 0 error + 0 warning |
| 测试 | 21 套测试全部通过（含 34 个 DB 测试 + 5 个 Step16 集成测试） |
| 数据库迁移 | 数据不丢失，向前兼容 |

---

## 十三、实现偏差记录

| 计划项 | 实际实现 | 说明 |
|--------|---------|------|
| `get_user_by_token` in `IDatabasePool` | **已移除** | Token 使用 HMAC 自包含方案，无需服务端状态查询 |
| 密码哈希：bcrypt | **OpenSSL EVP SHA-256 + 盐值** | 使用 `generate_salt()` / `hash_password()` |
| `STRAIGHT_JOIN` 查询优化 | **标准 JOIN** | 保持 MySQL/MariaDB 兼容性，避免锁表风险 |
| `get_file_records_by_music` | 方法命名差异 | 功能通过已有查询组合实现 |
| 路由编号：N1–N11 | **实际路由：register, logout, me, stream, search, delete, update_user, M1–M8** | 命名更语义化 |
| 分块读取（10.1 节代码示例） | 实现为 `build_206_headers` + `parse_range_header` 公共函数复用 | 更简洁 |

### Pipeline 结果

```
[Step 0] ✅ 需求分析 → [Step 1] ✅ 需求细化 → [Step 2] ✅ 编码
→ [Step 3] ✅ Lint（0 error, 0 warning, 0 style）
→ [Step 4] ✅ 编译（0 error, 0 warning）
→ [Step 5] ⏭️ CodeQL（服务器不可用，跳过）
→ [Step 6] ✅ 测试（21/21 全部通过）
→ [Step 7] ✅ 总结报告
```

### 测试修复

在 Step 6 测试阶段发现并修复了 `core/src/auth_service.cpp:131` 中 `validate_token` 解析 payload 的偏移量计算错误（`uid_pos + 4` 应为 `uid_pos + 3`，同理 role/exp），导致多位数 user_id 仅解析出个位数。同时更新了 `test_database_pool.cpp` 中的 mock 查询列数以匹配新 schema（增加 `salt`、`music_id`、`uploaded_by`）。
