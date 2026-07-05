# 端到端验证计划

## 变更清单

### 1. core/src/main.cpp — 路由注册
- [ ] B2 修复: `load_config` 交换 parse_json_file / parse_cmd_args 顺序
- [ ] 新增 POST `/api/files/upload` handler
- [ ] 新增 GET `/api/files/:hash/download` handler
- [ ] `register_routes` 使用 `fs` 参数（移除 `/*fs*/`）

### 2. net/http/include/i_router.h
- [ ] 新增 `virtual bool path_exists(std::string_view path) const = 0;`

### 3. net/http/include/router.h
- [ ] 新增 `bool path_exists(std::string_view path) const override;`

### 4. net/http/src/router.cpp
- [ ] 实现 `path_exists`（搜索 trie 忽略 method）

### 5. net/http/src/http_server.cpp
- [ ] B5 修复: `handle_connection` 中 match 失败后调 `path_exists` → 405

### 6. verification/verify.sh — 验证脚本增强
- [ ] V7: 10 次 Keep-Alive（原 3 次）
- [ ] V12: 文件全生命周期（上传/元信息/下载/hash/大文件/重复上传/二进制）
- [ ] V14: 4 线程并发请求
- [ ] V15: 边界（空 body、PUT/DELETE 方法不匹配）

### 7. verification/README.md — 同步文档

---

## 接口定义

```cpp
// i_router.h 新增
virtual bool path_exists(std::string_view path) const = 0;

// upload handler
POST /api/files/upload → 201 {"hash":"<sha256>","size":N}
                       → 200 {"hash":"...","exists":true} (重复)
                       → 400 {"error":"empty body"}

// download handler
GET /api/files/:hash/download → 200 + octet-stream body
                              → 404 {"error":"file not found"}
```

---

## 验证用例

| ID | 操作 | 期望 |
|----|------|------|
| V7 | 10x GET /api/health Keep-Alive | 全部 200 |
| V12.1 | 上传 100B | 201 + hash + size=100 |
| V12.2 | 查元信息 | 200 + hash/size 匹配 |
| V12.3 | 下载比对 hash | sha256sum 一致 |
| V12.4 | 上传 256B（含 null） | 同上三步 |
| V12.5 | 上传 5MB | 201 + 下载 hash 一致 |
| V12.6 | 下载不存在 | 404 |
| V12.7 | 重复上传 | 200 + exists=true |
| V14 | 4 线程并发 GET | 全部 200 |
| V15.1 | POST 空 body | 不崩溃 |
| V15.2 | PUT /api/health | 非 200（405 或 404） |
| V15.3 | DELETE /api/health | 同上 |

---

## 质量门禁

- lint.sh --changed: 0/0/0
- test.sh: 20/20 passed
- 编译 0 error 0 warning
