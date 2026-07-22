# 修复方案：Boost MySQL Prepared Statement 查询失败 ✅ 已完成

## 问题

所有带 `?` 占位符的 SQL（prepared statement）在 `conn->query()` 中静默失败，返回 `nullopt`。不带参数的 SQL 正常。

## 根因

`execute_stmt` 及 `connect`/`ping`/`query`/`execute` 的 catch 块静默吞掉所有异常（`catch (...) { return false; }`），不输出任何诊断信息。无法区分是 `field_view` 生命周期、`bind()` API 签名不匹配还是 `caching_sha2_password` 认证问题。

## 修复内容

### 1. 诊断增强（所有 catch 块）

所有 4 组 catch 块添加 stderr 诊断输出，方便在真实 MySQL 环境下定位：
- `db/src/boost_mysql_connection.cpp:connect()` — catch 块
- `db/src/boost_mysql_connection.cpp:ping()` — catch 块
- `db/src/boost_mysql_connection.cpp:query()` — catch 块
- `db/src/boost_mysql_connection.cpp:execute()` — catch 块

### 2. bind API 兼容性调整

`execute_stmt` 的 `stmt.bind(fvs.begin(), fvs.end())` 在 Boost 1.83+ 可能不兼容迭代器对签名。
调整为 `stmt.bind(fvs.data(), fvs.size())` 适配 span-based 新签名。

## 验证

- [x] 诊断日志可输出到 stderr
- [x] Step 交付时全量回归：lint 0/0 + 编译 0/0 + test 37/37 + CodeQL 0/0
- [x] Step 交付时质量门禁全部通过

> 上述 37/37 是本问题修复交付时的历史快照，不代表当前测试规模。2026-07-22 执行 `bash scripts/test.sh` 的当前快照为：后端 Google Test 41/41、前端 Vitest 18 个测试文件与 71 个用例全部通过。当前完整门禁与部署验收见 [Step 17：前端体验与上传链路优化](step-17-frontend-optimization.md)。

## 历史根因排查（按当时优先级排序）

以下内容保留当时的候选原因与排查依据，用于解释修复过程，不是当前仍待执行的任务清单。

### 可能性 A：`field_view` 生命周期（最可能）

`mysql::field_view` 是**引用语义**（zero-copy），构造时只保存指向源数据的指针/引用。在 `execute_stmt` 中：

1. `fvs` 是局部 vector，元素通过 `mysql::string_view(p)` 引用 `params` 中的 `std::string`
2. `params` 是 `const std::vector<std::string>&`，活到函数结束
3. `stmt.bind(fvs.begin(), fvs.end())` 创建临时 executor
4. `conn_.execute(..., result)` 执行查询，此时 `fvs` 和 `params` 都有效

→ 生命周期理论上正确。但 Boost 1.83 的 `stmt.bind()` 签名和内部实现需要确认。

### 可能性 B：Boost 1.83 `statement::bind()` API 不匹配

Boost MySQL 的 `statement::bind()` 在旧版本接受迭代器对，但 1.83 可能已改为接受 `span` 或 `range`：

| Boost 版本 | bind 签名 |
|-----------|-----------|
| < 1.82 | `bind(InputIterator first, InputIterator last)` |
| >= 1.82 | `bind(const field_view&)` 或 `bind(const Span<const field_view>&)` |

### 可能性 C：MySQL 8 `caching_sha2_password` 认证插件兼容性

`caching_sha2_password`（MySQL 8+ 默认）需要 SSL 或 `CLIENT_PLUGIN_AUTH` 标志，Boost MySQL 连接时未协商导致 prepared statement 失败。

## 历史排查过程与已验证结果

### 1. 添加详细诊断（定位到底在哪步抛异常）

最小侵入地在 `boost_mysql_connection.cpp` 的 catch 块打印 `e.what()` 和 `e.get_diagnostics().server_message()`，stderr 输出：

```cpp
} catch (const mysql::error_with_diagnostics& e) {
    std::cerr << "[mysql] query error: " << e.what()
              << " | server: " << e.get_diagnostics().server_message() << std::endl;
    return std::nullopt;
} catch (const std::exception& e) {
    std::cerr << "[mysql] query exception: " << e.what() << std::endl;
    return std::nullopt;
}
```

当时通过编译并执行登录请求获取具体数据库错误。该详细诊断不是一次性日志：`error_with_diagnostics` 的 `what()` 与服务端 `server_message()` 输出后续继续保留，便于定位真实 MySQL 环境中的失败原因。

### 2. 当时根据错误选择修复方向

| 错误信息 | 根因 | 修复 |
|---------|------|------|
| `bind` 相关编译错误或运行时参数不匹配 | API 接口变了 | 改用 `field_view` 数组 + `bind(const field_view*, size_t)` 或逐个 bind |
| `caching_sha2_password` 或认证错误 | 认证插件不兼容 | 连接时指定 `mysql_native_password` 或在 MySQL 侧改用户认证 |
| `Column count mismatch` / 语法错误 | statement 参数绑定错位 | 检查 `execute_stmt` 中 `fvs` 构造方式 |

### 3. 修复后的历史验证结果

- [x] 当时验证：注册新用户可返回 token
- [x] 当时验证：使用用户凭据登录可返回 token
- [x] 当时验证：获取用户信息 `/api/auth/me` 成功
- [x] 当时执行全量回归；交付快照为 37/37，当前测试快照见上文
- [x] 保留详细数据库失败诊断并重新编译；诊断能力没有从后端移除

## 回退方案

当时准备的降级方案是：对简单 SQL 改用字符串拼接 + `conn_.execute(sql, result)` 不带参数（需防 SQL 注入的项目单独加固）。该方案未采用，保留在此仅供历史追溯；**不推荐用于当前实现。**
