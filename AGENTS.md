# 项目规则

## 语言
- 所有回复、报告、注释、文档必须使用中文
- 不允许用英文回复（除非是代码本身）

## 技术栈
- 编程语言：C++20
- 编译器：g++（Linux 原生）
- 构建工具：xmake v3
- 测试框架：Google Test（googletest）
- 调试器：GDB
- 静态分析：clang-tidy + cppcheck
- 语义分析：CodeQL（Docker 容器，localhost）

## CodeQL 集成

### 前置条件
- CodeQL 容器（Docker）已在局域网某台机器上运行
- 端口固定：**8080**
- IP 不固定：每次分析前由用户提供 CodeQL 服务器 IP
- 环境变量 `CODEQL_SERVER_URL` 根据用户输入设置

### API 端点
| 端点 | 方法 | 说明 |
|---|---|---|
| `/analyze` | POST | 提交源码分析 |
| `/result/<task_id>` | GET | 获取分析结果 |

### 数据传输规范

1. **生成编译数据库**
   ```
   xmake project -k compile_commands
   ```

2. **预处理 compile_commands.json**
   - 过滤外部依赖（如 gtest）条目（判断条件：`file` 为绝对路径如 `/usr/...`）
   - 将 `directory` 统一改为 `"."`
   - 将 `file` 路径中的反斜杠 `\` 转为正斜杠 `/`

3. **打包源码**
   - 必要内容：`src/`、`tests/`、`xmake.lua`
   - 排除：`*.o`、`*.obj`、`*.exe`、`__pycache__`、`.xmake/`、`build/`
   - 格式：tar.gz

4. **发送请求**（使用 curl）
   ```bash
   read -p "请输入 CodeQL 服务器 IP: " ip
   export CODEQL_SERVER_URL="http://${ip}:8080"
   curl -X POST "$CODEQL_SERVER_URL/analyze" \
     -F "source=@source.tar.gz;type=application/gzip" \
     -F "compile_commands=@compile_commands_fixed.json;type=application/json"
   ```

5. **解析 SARIF 响应**
   - 读取 `result.result.runs[0].results`
   - 遍历每个 result，提取 `ruleId`、`level`/`properties.severity`、`message.text`、`locations`
   - 检查 `invocations[0].executionSuccessful`
   - 检查 `invocations[0].toolExecutionNotifications` 获取提取状态

6. **质量门禁**
   - 0 critical + 0 high severity 视为通过
   - 若发现问题 → 修复代码 → 重新从第 3 步开始（LOOP）

## xmake 常用命令
| 命令 | 说明 |
|---|---|
| `xmake` | 编译（debug 模式） |
| `xmake f -c && xmake` | 重新配置并编译 |
| `xmake run` | 运行默认目标 |
| `xmake run <target>` | 运行指定目标 |
| `xmake test` | 运行所有测试 |
| `xmake test -f <case>` | 运行指定测试用例 |
| `xmake build -b <target>` | 构建指定目标 |
| `xmake clean` | 清理构建 |
| `xmake run -d` | GDB 调试运行 |
| `xmake project -k compile_commands` | 生成 compile_commands.json |

## 项目结构
```
project/
├── xmake.lua
├── opencode.jsonc
├── tui.json
├── AGENTS.md
├── .opencode/
│   ├── agents/
│   │   └── debug-cpp.md
│   └── commands/
├── src/
│   └── <module>/
│       ├── *.cpp
│       └── *.hpp
├── include/
└── tests/
    └── test_*.cpp
```

## 编码规范
- 类名：PascalCase（ThreadPool）
- 函数/变量：snake_case（init_pool）
- 常量：UPPER_SNAKE_CASE（MAX_THREADS）
- 头文件保护：#pragma once
- 优先使用 C++20 标准库
- RAII 优先

## 质量门禁（严格模式）
- clang-tidy：0 error + 0 warning + 0 style
- cppcheck --enable=all：0 error + 0 warning + 0 style + 0 performance
- CodeQL：0 critical + 0 high severity
- 编译：0 error + 0 warning
- 测试：100% 通过
