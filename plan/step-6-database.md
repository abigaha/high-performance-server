# Step 6：database 数据库连接池

> **状态**：← 当前关卡
> **优先级**：P0

## 背景

音乐软件需要持久化存储用户信息、下载日志、文件哈希与存储地址。需实现 MySQL 数据库连接池模块，为上层业务提供统一的数据库访问接口。

## 功能点

| # | 功能点 | 优先级 | 说明 |
|---|--------|--------|------|
| F1 | **IDatabasePool 抽象接口** | P0 | 数据库操作抽象层 |
| F2 | **数据模型定义** | P0 | User / DownloadLog / FileMeta / DbConfig |
| F3 | **连接池实现** | P0 | mutex + queue 固定大小池 |
| F4 | **MySQL C API 封装** | P0 | libmysqlclient 封装 |
| F5 | **用户 CRUD** | P0 | get_user / create_user |
| F6 | **下载日志** | P1 | log_download / get_download_history |
| F7 | **文件元信息 CRUD** | P0 | store_file_meta / get_file_meta |
| F8 | **SQL schema** | P0 | DDL 脚本 |

## 接口设计

```cpp
struct DbConfig {
  std::string host = "127.0.0.1";
  uint16_t port = 3306;
  std::string user = "root";
  std::string password;
  std::string database = "music_server";
  std::size_t pool_size = 10;
  uint32_t connect_timeout_ms = 3000;
  uint32_t read_timeout_ms = 5000;
};

struct User {
  int64_t user_id{0};
  std::string username;
  std::string password_hash;
  std::string email;
  std::string created_at;
};

struct DownloadLog {
  int64_t log_id{0};
  int64_t user_id{0};
  std::string file_hash;
  std::string downloaded_at;
};

struct FileMeta {
  std::string file_hash;
  std::string file_path;
  std::size_t file_size{0};
  std::string content_type;
  std::string created_at;
};

class IDatabasePool {
public:
  virtual ~IDatabasePool() = default;
  virtual bool init(const DbConfig& config) = 0;
  virtual void close() = 0;
  virtual std::optional<User> get_user(int64_t user_id) = 0;
  virtual bool create_user(const User& user) = 0;
  virtual bool log_download(const DownloadLog& log) = 0;
  virtual std::vector<DownloadLog> get_download_history(int64_t user_id) = 0;
  virtual bool store_file_meta(const FileMeta& meta) = 0;
  virtual std::optional<FileMeta> get_file_meta(const std::string& hash) = 0;
};
```

## 连接池设计

```
DatabasePool : IDatabasePool
├── init() → 建立 pool_size 个 MySQL 连接
├── get_connection() → MYSQL*（超时等待）
├── release_connection(MYSQL*) → 放回队列
├── ping() 健康检查
└── close() → 关闭所有连接
```

## 文件清单

| 操作 | 路径 |
|------|------|
| 创建 | `db/include/db_config.h` |
| 创建 | `db/include/models.h` |
| 创建 | `db/include/idatabase_pool.h` |
| 创建 | `db/include/database_pool.h` |
| 创建 | `db/src/database_pool.cpp` |
| 创建 | `db/xmake.lua` |
| 创建 | `tests/test_database_pool.cpp` |
| 修改 | 顶层 `xmake.lua`（添加 `includes("db")`）|

## 测试用例

| # | 说明 | 类型 |
|---|------|------|
| T1 | DbConfig 默认值 | 单元 |
| T2 | User 模型字段 | 单元 |
| T3 | DownloadLog 模型字段 | 单元 |
| T4 | FileMeta 模型字段 | 单元 |
| T5 | 连接池创建销毁（mock）| 单元 |
| T6 | 连接获取归还（mock）| 单元 |
| T7 | 连接超时（mock）| 单元 |
| T8 | get_user（mock）| 单元 |
| T9 | create_user（mock）| 单元 |
| T10 | log_download + get_download_history（mock）| 单元 |
| T11 | store_file_meta + get_file_meta（mock）| 单元 |
