# high-performance-server

# 开发环境

Ubuntu 24.04
g++-14
cmake + ninja
clang-format-14

# 文件路径说明

core：日志模块和main函数
build：编译文件夹
lib：我编译的库文件
bin：编译后的可执行文件

# 日志系统

## 概述

高性能服务器项目实现了一套灵活且高效的日志系统，参考 Log4j 架构设计，由 Logger、LogFormatter、LogAppender 等核心组件组成。支持多级别日志、自定义格式化、多输出目标等功能。

## 架构设计

### 核心类结构与联系

```
                    ┌─────────────┐
                    │   Logger    │ (日志主类)
                    │   (名称)    │
                    │   (级别)    │
                    │  (Appenders)│
                    └──────┬──────┘
                           │
                ┌──────────┴──────────┐
                │                     │
         ┌──────▼──────┐      ┌──────▼──────┐
         │  StdoutApp  │      │  FileApp    │
         │(控制台输出) │      │  (文件输出) │
         └──────┬──────┘      └──────┬──────┘
                │                     │
                └──────────┬──────────┘
                           │
                    ┌──────▼─────────┐
                    │   Formatter    │ (格式化器)
                    │   (模式串)     │
                    │  (FormatItems) │
                    └────────────────┘
                           ▲
                           │ uses
                    ┌──────┴─────────┐
                    │   LogEvent     │ (日志事件)
                    │  (文件、行号)  │
                    │  (时间、线程)  │
                    │  (日志内容)    │
                    └────────────────┘
```

### 工作流程

1. **创建日志事件**：调用 `Logger::debug/info/warn/error/fatal()` 方法
2. **自动捕获上下文**：使用 C++20 `source_location` 自动获取文件名和行号
3. **构建 LogEvent**：Logger 工厂方法填充时间、线程ID、运行时间等信息
4. **级别过滤**：比较日志级别和 Logger 设置的最小级别
5. **分发到 Appender**：通过所有已注册的 Appender 输出
6. **格式化输出**：Appender 调用 LogFormatter 进行格式化
7. **最终输出**：输出到控制台或文件

---

## 核心类详解

### 1. LogLevel 枚举

```cpp
enum class LogLevel { DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4, FATAL = 5 };
```

**功能**：定义 5 个日志级别，按优先级递增。

| 级别 | 值 | 用途 |
|------|-----|------|
| DEBUG | 1 | 调试信息（最详细） |
| INFO | 2 | 普通信息 |
| WARN | 3 | 警告信息 |
| ERROR | 4 | 错误信息 |
| FATAL | 5 | 严重错误（最关键） |

---

### 2. LogEvent 类

**职责**：封装单条日志事件，包含日志发生的上下文信息。

#### 构造函数

```cpp
LogEvent::LogEvent(const std::string& content, 
                   const std::source_location& loc = std::source_location::current());
```

**参数**：
- `content`：日志内容（消息文本）
- `loc`：源位置信息（默认为调用点，由编译器自动填充）

**初始化流程**：
1. 从 `source_location` 提取文件名和行号
2. 通过 `std::this_thread::get_id()` 获取当前线程ID（转换为 uint32_t hash）
3. 使用 `std::chrono::system_clock` 获取系统时间戳
4. 计算程序启动至今的运行时间（毫秒）

#### 访问器方法

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `getFile()` | `const char*` | 源文件路径 |
| `getLine()` | `int32_t` | 源代码行号 |
| `getThreadId()` | `uint32_t` | 线程ID（hash值） |
| `getElapse()` | `uint32_t` | 程序运行时间（毫秒） |
| `getTime()` | `uint64_t` | 系统时间戳 |
| `getContent()` | `const std::string&` | 日志内容 |

---

### 3. LogFormatter 类

**职责**：将日志事件格式化为可读的字符串。使用模式驱动的格式化方式。

#### 构造函数

```cpp
LogFormatter::LogFormatter(const std::string& pattern);
```

**参数**：
- `pattern`：格式化模式串，使用占位符定义输出格式

#### 格式化模式支持

| 占位符 | 说明 | 示例 |
|--------|------|------|
| `%m` | 日志消息内容 | 日志文本 |
| `%p` | 日志级别 | DEBUG, INFO, WARN, ERROR, FATAL |
| `%d{format}` | 日期时间 | %d{%Y-%m-%d %H:%M:%S} → 2026-05-20 15:45:24 |
| `%f` | 源文件名（含路径） | /path/to/file.cpp |
| `%l` | 源代码行号 | 42 |
| `%t` | 线程ID | 12345 |
| `%c` | Logger名称 | root |
| `%n` | 换行符 | \n |
| 普通文本 | 直接输出 | 任何其他字符 |

#### 使用示例

```cpp
// 模式示例 1：简洁格式
auto fmt1 = std::make_shared<LogFormatter>("[%p] %m%n");
// 输出：[INFO] This is a message

// 模式示例 2：完整格式
auto fmt2 = std::make_shared<LogFormatter>("%d{%Y-%m-%d %H:%M:%S} [%p] %f:%l %m%n");
// 输出：2026-05-20 15:45:24 [ERROR] main.cpp:42 Connection failed

// 模式示例 3：自定义格式
auto fmt3 = std::make_shared<LogFormatter>("<%p> [%t] %m%n");
// 输出：<WARN> [12345] Low memory
```

#### 核心方法

```cpp
std::string format(LogLevel level, const LogEvent::ptr event);
```

返回格式化后的日志字符串。内部通过遍历所有 FormatItem 依次格式化。

#### 内部实现细节

- **模式解析**（`init()` 方法）：
  - 逐字符扫描模式串
  - 识别 `%x` 或 `%x{...}` 格式
  - 构建 FormatItem 对象列表
  
- **FormatItem 层次**：
  - 基类 `FormatItem`（虚基类）
  - 子类包括：`MessageFormatItem`、`LevelFormatItem`、`DateTimeFormatItem` 等
  - 每个子类实现 `format()` 方法，负责单一职责

---

### 4. LogAppender 基类

**职责**：定义日志输出的接口。实现不同的子类可输出到不同目标。

#### 核心接口

```cpp
class LogAppender {
public:
    virtual void log(LogLevel level, const LogEvent::ptr event) = 0;
    void setFormatter(LogFormatter::ptr val) { formatter_ = val; }
    LogFormatter::ptr getFormatter() const { return formatter_; }
protected:
    LogLevel level_ = LogLevel::DEBUG;
    LogFormatter::ptr formatter_;
};
```

| 成员 | 类型 | 说明 |
|------|------|------|
| `formatter_` | LogFormatter::ptr | 格式化器实例 |
| `level_` | LogLevel | Appender 的最小日志级别（可独立设置） |

#### 关键方法

- **`setFormatter(ptr)`**：为此 Appender 设置格式化器
- **`log(level, event)`**：纯虚方法，子类必须实现

---

### 5. StdoutLogAppender 类

**职责**：将日志输出到标准输出（控制台）。

```cpp
class StdoutLogAppender : public LogAppender {
public:
    void log(LogLevel level, const LogEvent::ptr event) override;
};
```

#### 实现细节

1. 检查 `formatter_` 非空
2. 调用 `formatter_->format()` 获取格式化字符串
3. 通过 `std::cout` 输出到控制台

#### 特点

- 输出到 stdout，便于本地开发和调试
- 支持彩色输出（可在后续扩展）
- 无文件 I/O 开销

---

### 6. FileLogAppender 类

**职责**：将日志输出到文件。

```cpp
class FileLogAppender : public LogAppender {
public:
    FileLogAppender(const std::string& filename);
    void log(LogLevel level, const LogEvent::ptr event) override;
    bool reopen();
private:
    std::string filename_;
    std::ofstream filestream_;
};
```

#### 构造函数

```cpp
FileLogAppender::FileLogAppender(const std::string& filename);
```

- `filename`：日志文件路径（绝对或相对）
- 自动调用 `reopen()` 打开文件

#### 核心方法

| 方法 | 说明 |
|------|------|
| `reopen()` | 关闭旧文件流，打开新文件流（追加模式） |
| `log()` | 格式化日志并写入文件，然后 flush |

#### 特性

- **追加模式**（`std::ios::app`）：日志持续追加，不覆盖已有内容
- **自动 flush**：确保日志立即写入磁盘（支持程序崩溃时的日志保存）
- **文件重新打开**：支持日志轮转或外部日志轮换工具

---

### 7. Logger 类

**职责**：日志主类。管理 Appender、执行日志级别过滤、提供便捷的日志接口。

#### 构造函数

```cpp
Logger::Logger(const std::string& name = "root");
```

- `name`：Logger 实例名称，用于标识日志来源（如 "db", "http", "root"）

#### LogEvent 工厂方法

```cpp
LogEvent::ptr Logger::createEvent(const std::string& content, 
                                  const std::source_location& loc = std::source_location::current());
```

**功能**：
- 创建 LogEvent 实例
- 自动捕获调用点的文件名和行号
- 自动填充时间、线程ID、运行时间等上下文信息

**设计**：使用工厂方法集中管理 LogEvent 的创建，确保信息完整性。

#### 便捷日志方法

```cpp
void debug(const std::string& msg, const std::source_location& loc = std::source_location::current());
void info(const std::string& msg, const std::source_location& loc = std::source_location::current());
void warn(const std::string& msg, const std::source_location& loc = std::source_location::current());
void error(const std::string& msg, const std::source_location& loc = std::source_location::current());
void fatal(const std::string& msg, const std::source_location& loc = std::source_location::current());
```

**工作流程**：
1. 通过 `createEvent()` 创建 LogEvent
2. 调用 `log(level, event)` 分发日志

#### 核心日志方法

```cpp
void Logger::log(LogLevel level, const LogEvent::ptr event);
```

**日志级别过滤**：
- 比较 `level >= level_`
- 仅当日志级别 ≥ Logger 设置的最小级别时才输出
- 遍历所有 Appender，逐一输出

#### Appender 管理

```cpp
void addAppender(const LogAppender::ptr appender);
void delAppender(const LogAppender::ptr appender);
```

| 方法 | 功能 |
|------|------|
| `addAppender()` | 添加 Appender 到列表（支持多个） |
| `delAppender()` | 从列表中移除指定 Appender |

#### 日志级别控制

```cpp
LogLevel getLevel() const { return level_; }
void setLevel(LogLevel val) { level_ = val; }
```

| 方法 | 功能 |
|------|------|
| `setLevel()` | 设置 Logger 的最小日志级别 |
| `getLevel()` | 获取当前最小日志级别 |

**示例**：
```cpp
logger->setLevel(LogLevel::WARN);
logger->debug("Skipped");  // 不输出（DEBUG < WARN）
logger->warn("Output");    // 输出（WARN >= WARN）
```

---

## 使用指南

### 1. 基础使用

#### 步骤 1：创建 Logger

```cpp
#include "logger.h"
using namespace hps;

auto logger = std::make_shared<Logger>("app");
```

#### 步骤 2：创建 Appender

```cpp
// 控制台输出
auto stdout_app = std::make_shared<StdoutLogAppender>();

// 文件输出
auto file_app = std::make_shared<FileLogAppender>("/var/log/app.log");
```

#### 步骤 3：创建并配置 Formatter

```cpp
auto formatter = std::make_shared<LogFormatter>(
    "%d{%Y-%m-%d %H:%M:%S} [%p] %f:%l %m%n"
);
stdout_app->setFormatter(formatter);
file_app->setFormatter(formatter);
```

#### 步骤 4：添加 Appender 到 Logger

```cpp
logger->addAppender(stdout_app);
logger->addAppender(file_app);
```

#### 步骤 5：记录日志

```cpp
logger->debug("Application started");
logger->info("Server listening on port 8080");
logger->warn("Cache miss rate: 45%");
logger->error("Connection timeout");
logger->fatal("Database unavailable");
```

### 2. 完整示例

```cpp
#include "logger.h"

using namespace hps;

int main() {
    // 创建 Logger
    auto logger = std::make_shared<Logger>("server");
    
    // 创建控制台 Appender
    auto console = std::make_shared<StdoutLogAppender>();
    auto console_fmt = std::make_shared<LogFormatter>("[%p] %m%n");
    console->setFormatter(console_fmt);
    logger->addAppender(console);
    
    // 创建文件 Appender
    auto file = std::make_shared<FileLogAppender>("server.log");
    auto file_fmt = std::make_shared<LogFormatter>(
        "%d{%Y-%m-%d %H:%M:%S} [%p] [%t] %f:%l %m%n"
    );
    file->setFormatter(file_fmt);
    logger->addAppender(file);
    
    // 设置日志级别
    logger->setLevel(LogLevel::INFO);
    
    // 记录日志
    logger->debug("Debug info (skipped)");
    logger->info("Server starting...");
    logger->warn("Memory usage at 80%");
    logger->error("Failed to connect to database");
    
    return 0;
}
```

**输出结果**：

控制台：
```
[INFO] Server starting...
[WARN] Memory usage at 80%
[ERROR] Failed to connect to database
```

文件 (server.log)：
```
2026-05-20 15:45:24 [INFO] [12345] main.cpp:28 Server starting...
2026-05-20 15:45:25 [WARN] [12345] main.cpp:29 Memory usage at 80%
2026-05-20 15:45:26 [ERROR] [12345] main.cpp:30 Failed to connect to database
```

### 3. 高级特性

#### 多个 Logger 实例

```cpp
auto db_logger = std::make_shared<Logger>("database");
auto http_logger = std::make_shared<Logger>("http");

db_logger->addAppender(file_app);
http_logger->addAppender(console_app);
```

#### 独立配置每个 Appender

```cpp
auto strict_file = std::make_shared<FileLogAppender>("errors.log");
strict_file->setFormatter(error_formatter);
strict_file->setLevel(LogLevel::ERROR);  // 仅记录 ERROR 及以上

logger->addAppender(strict_file);
// 此 appender 仅输出 ERROR 和 FATAL
```

#### 日志级别动态调整

```cpp
// 开发环境：输出所有日志
logger->setLevel(LogLevel::DEBUG);

// 生产环境：仅输出警告及以上
logger->setLevel(LogLevel::WARN);
```

#### 自定义格式

常用格式模式：

```cpp
// 简洁格式
"%p %m%n"
// 输出：INFO This is a message

// 标准格式
"%d{%Y-%m-%d %H:%M:%S} [%p] %m%n"
// 输出：2026-05-20 15:45:24 [ERROR] This is a message

// 详细格式（包含文件和线程）
"%d{%Y-%m-%d %H:%M:%S} [%p] [%t] %f:%l %m%n"
// 输出：2026-05-20 15:45:24 [ERROR] [12345] main.cpp:42 This is a message

// 自定义格式
"<%p> %d{%H:%M:%S} - %m%n"
// 输出：<WARN> 15:45:24 - Low memory available
```

### 4. 性能考虑

- **日志级别过滤**：在调用 `log()` 前进行，避免不必要的 LogEvent 创建
- **Flush 行为**：FileLogAppender 在每次写入后自动 flush，确保数据安全但有性能成本
- **多 Appender**：如无特殊需求，建议不超过 3-4 个 Appender

### 5. 常见问题

**Q: 如何禁用某个 Appender？**

A: 调用 `logger->delAppender(appender)` 或暂时不创建该 Appender。

**Q: 如何支持多个日志文件？**

A: 创建多个 FileLogAppender，配置不同的文件路径和格式。

**Q: 是否支持日志轮转？**

A: 当前实现不支持自动轮转，可由外部脚本（如 logrotate）管理文件轮转，配合 `reopen()` 方法。

**Q: 如何让不同模块使用不同的日志配置？**

A: 为每个模块创建独立的 Logger 实例，配置不同的 Appender 和格式。

---

## 设计总结

### 优势

1. **灵活性**：支持多个 Appender 和自定义格式
2. **自动上下文**：无需手动传入文件名/行号（C++20）
3. **级别过滤**：减少日志处理开销
4. **扩展性**：易于添加新的 FormatItem 或 Appender 类型
5. **线程安全基础**：各组件设计支持后续加锁实现线程安全

### 后续扩展方向

- 添加 RollingFileAppender（自动日志轮转）
- 添加 AsyncAppender（异步日志输出）
- 添加 SocketAppender（网络日志收集）
- 实现线程安全的锁机制
- 支持不同 Appender 的独立日志级别


# 线程池 

# 连接池

# 协程库封装

# socket封装

# http协议开发

# 分布式协议

---

## 附录：完整 API 参考表

### LogEvent 完整接口

| 成员函数 | 签名 | 说明 |
|---------|------|------|
| 构造函数 | `LogEvent(const std::string& content, const std::source_location& loc = std::source_location::current())` | 创建日志事件，自动捕获源位置 |
| `getFile()` | `const char*` | 返回源文件路径 |
| `getLine()` | `int32_t` | 返回源代码行号 |
| `getThreadId()` | `uint32_t` | 返回线程ID（哈希值） |
| `getElapse()` | `uint32_t` | 返回程序启动至今的毫秒数 |
| `getTime()` | `uint64_t` | 返回系统时间戳（秒） |
| `getContent()` | `const std::string&` | 返回日志内容 |

### LogFormatter 完整接口

| 成员函数 | 签名 | 说明 |
|---------|------|------|
| 构造函数 | `LogFormatter(const std::string& pattern)` | 创建格式化器，解析模式串 |
| `format()` | `std::string format(LogLevel level, const LogEvent::ptr event)` | 返回格式化后的日志字符串 |

### LogAppender 完整接口

| 成员函数 | 签名 | 说明 |
|---------|------|------|
| `setFormatter()` | `void setFormatter(LogFormatter::ptr val)` | 设置格式化器 |
| `getFormatter()` | `LogFormatter::ptr getFormatter() const` | 获取格式化器 |
| `log()` | `virtual void log(LogLevel level, const LogEvent::ptr event) = 0` | 纯虚函数，子类必须实现 |

### StdoutLogAppender 完整接口

| 成员函数 | 签名 | 说明 |
|---------|------|------|
| `log()` | `void log(LogLevel level, const LogEvent::ptr event) override` | 输出到标准输出 |

### FileLogAppender 完整接口

| 成员函数 | 签名 | 说明 |
|---------|------|------|
| 构造函数 | `FileLogAppender(const std::string& filename)` | 创建文件输出器 |
| `log()` | `void log(LogLevel level, const LogEvent::ptr event) override` | 输出到文件 |
| `reopen()` | `bool reopen()` | 重新打开日志文件（支持日志轮转） |

### Logger 完整接口

| 成员函数 | 签名 | 说明 |
|---------|------|------|
| 构造函数 | `Logger(const std::string& name = "root")` | 创建日志器实例 |
| `createEvent()` | `LogEvent::ptr createEvent(const std::string& content, const std::source_location& loc = std::source_location::current())` | 创建日志事件（工厂方法） |
| `debug()` | `void debug(const std::string& msg, const std::source_location& loc = std::source_location::current())` | 记录DEBUG级别日志 |
| `info()` | `void info(const std::string& msg, const std::source_location& loc = std::source_location::current())` | 记录INFO级别日志 |
| `warn()` | `void warn(const std::string& msg, const std::source_location& loc = std::source_location::current())` | 记录WARN级别日志 |
| `error()` | `void error(const std::string& msg, const std::source_location& loc = std::source_location::current())` | 记录ERROR级别日志 |
| `fatal()` | `void fatal(const std::string& msg, const std::source_location& loc = std::source_location::current())` | 记录FATAL级别日志 |
| `log()` | `void log(LogLevel level, const LogEvent::ptr event)` | 根据级别分发日志到所有Appender |
| `addAppender()` | `void addAppender(const LogAppender::ptr appender)` | 添加输出目标 |
| `delAppender()` | `void delAppender(const LogAppender::ptr appender)` | 移除输出目标 |
| `setLevel()` | `void setLevel(LogLevel val)` | 设置最小日志级别 |
| `getLevel()` | `LogLevel getLevel() const` | 获取当前最小日志级别 |

---

## 类关系图（文本表示）

```
┌──────────────────────────────────────────────────────────────┐
│                      应用程序 (使用者)                         │
└──────────────┬───────────────────────────────────────────────┘
               │
               │ 调用 logger->debug("msg"), etc.
               ▼
     ┌─────────────────────┐
     │     Logger          │◄───────── 日志主类
     │  (名称、级别、App列表)│
     └──────────┬──────────┘
                │
                │ 创建 LogEvent
                ▼
    ┌──────────────────────┐
    │    LogEvent          │◄───────── 日志事件
    │  (内容、位置、时间)    │
    └──────┬───────────────┘
           │
           │ 分发给所有 Appender
           │
    ┌──────┴──────┬──────────┬────────┐
    │             │          │        │
    ▼             ▼          ▼        ▼
┌────────┐  ┌───────────┐ ┌─────┐ ┌────────┐
│ Stdout │  │ FileApp   │ │网络  │ │异步App │
│Appender│  │ (未实现)   │ │(未实) │ │(未实)  │
└────┬───┘  └─────┬─────┘ └─────┘ └────────┘
     │           │
     │ 调用 formatter->format()
     │           │
     └─────┬─────┘
           ▼
     ┌────────────────────┐
     │  LogFormatter      │◄───────── 格式化器
     │ (模式解析、FormatItems)│
     └────────────────────┘
           │
           │ 遍历所有 FormatItem
           │
    ┌──────┴────┬─────────┬──────────┬──────┬──────┐
    │           │         │          │      │      │
    ▼           ▼         ▼          ▼      ▼      ▼
┌──────┐ ┌────────┐ ┌─────────┐ ┌─────┐ ┌──┐ ┌────┐
│Message│ │LevelItem│ │DateTime │ │File │ │Ln│ │Thr │
│Item   │ │         │ │Item     │ │Name │ │  │ │ID  │
└──────┘ └────────┘ └─────────┘ └─────┘ └──┘ └────┘
```

---

## 数据流示例

以 `logger->info("Server started");` 为例：

```
1. 调用点：main.cpp:42
   │
   ▼
2. Logger::info("Server started", source_location@main.cpp:42)
   │
   ▼
3. Logger::createEvent("Server started", source_location@main.cpp:42)
   │
   ▼
4. LogEvent 构造，填充：
   - file_    = "main.cpp"
   - line_    = 42
   - threadId = 12345
   - time_    = 1717776324  (2026-05-20 15:45:24)
   - elapse_  = 1234        (程序运行 1.234 秒)
   - content_ = "Server started"
   │
   ▼
5. Logger::log(LogLevel::INFO, event_ptr)
   │
   ├─ 检查 LogLevel::INFO (2) >= Logger::level_ (DEBUG=1) ✓
   │
   ├─ 遍历所有 Appender：
   │
   ├─ Appender 1 (Stdout)：
   │   └─ StdoutLogAppender::log()
   │      └─ formatter->format(INFO, event_ptr)
   │         └─ 遍历 FormatItems 处理模式 "%d{%Y-%m-%d %H:%M:%S} [%p] %f:%l %m%n"
   │            ├─ DateTimeFormatItem::format()  → "2026-05-20 15:45:24"
   │            ├─ StringFormatItem(" [")         → " ["
   │            ├─ LevelFormatItem::format()      → "INFO"
   │            ├─ StringFormatItem("] ")         → "] "
   │            ├─ FileNameFormatItem::format()   → "main.cpp"
   │            ├─ StringFormatItem(":")          → ":"
   │            ├─ LineFormatItem::format()       → "42"
   │            ├─ StringFormatItem(" ")          → " "
   │            ├─ MessageFormatItem::format()    → "Server started"
   │            └─ NewLineFormatItem::format()    → "\n"
   │         └─ 返回: "2026-05-20 15:45:24 [INFO] main.cpp:42 Server started\n"
   │      └─ std::cout << "2026-05-20 15:45:24 [INFO] main.cpp:42 Server started\n"
   │      └─ 输出到控制台
   │
   └─ Appender 2 (File)：
      └─ FileLogAppender::log()
         └─ formatter->format(INFO, event_ptr) ...（同上）
         └─ filestream_ << "2026-05-20 15:45:24 [INFO] main.cpp:42 Server started\n"
         └─ filestream_.flush()
         └─ 写入到 server.log
```

---

## 类之间的依赖关系

```
强依赖（组合）：
  Logger *---* LogAppender      （Logger 持有多个 Appender）
  LogAppender *---1 LogFormatter （每个 Appender 有一个 Formatter）
  LogFormatter *---* FormatItem  （Formatter 持有多个 FormatItem）

弱依赖（使用）：
  Logger ---> LogEvent         （Logger 创建 LogEvent）
  LogAppender ---> LogEvent    （Appender 接收并处理 LogEvent）
  LogFormatter ---> LogEvent   （Formatter 读取 LogEvent 字段）
  FormatItem ---> LogEvent     （FormatItem 读取 LogEvent 字段）

继承关系：
  LogAppender
    ├─ StdoutLogAppender
    └─ FileLogAppender
  
  FormatItem
    ├─ MessageFormatItem
    ├─ LevelFormatItem
    ├─ ElapseFormatItem
    ├─ NameFormatItem
    ├─ ThreadIdFormatItem
    ├─ DateTimeFormatItem
    ├─ NewLineFormatItem
    ├─ StringFormatItem
    ├─ LineFormatItem
    ├─ FileNameFormatItem
    └─ PatternErrorFormatItem
```

---

## 总体架构说明

### 分层设计

```
┌─────────────────────────────────┐
│     应用层 (Application)        │  ← 使用 logger->debug/info/warn/error/fatal()
├─────────────────────────────────┤
│      Logger 层 (日志主类)       │  ← 负责级别过滤、Appender 管理、LogEvent 创建
├─────────────────────────────────┤
│   LogAppender 层 (输出抽象)     │  ← Stdout/File 等输出目标
├─────────────────────────────────┤
│  LogFormatter 层 (格式化抽象)   │  ← 模式解析、字符串拼接
├─────────────────────────────────┤
│   FormatItem 层 (格式项)        │  ← 各种格式元素的渲染
├─────────────────────────────────┤
│  LogEvent 层 (事件载体)         │  ← 封装日志上下文
├─────────────────────────────────┤
│  C++ 标准库和系统 API           │  ← chrono, thread, iostream, ctime
└─────────────────────────────────┘
```

### 关键设计原则

1. **单一职责原则**：每个类只负责一项功能
   - `Logger`：日志流程控制和级别管理
   - `LogAppender`：输出目标管理
   - `LogFormatter`：格式化逻辑
   - `FormatItem`：单个格式元素的渲染

2. **开闭原则**：对扩展开放，对修改关闭
   - 可扩展 FormatItem 子类（支持新的格式占位符）
   - 可扩展 LogAppender 子类（支持新的输出目标）
   - 无需修改现有类

3. **依赖倒置原则**：依赖抽象而非具体实现
   - Logger 依赖 LogAppender 基类（不关心具体子类）
   - LogAppender 依赖 LogFormatter 基类（允许不同的实现）

4. **工厂方法模式**：
   - Logger::createEvent() 集中管理 LogEvent 创建
   - 确保所有日志事件信息完整和一致

---

## 性能特性

| 特性 | 说明 | 优势 |
|------|------|------|
| 早期过滤 | 在 Logger::log() 检查级别 | 避免不必要的 LogEvent 创建 |
| 延迟格式化 | 仅在输出时格式化 | 输出少的日志几乎无成本 |
| 批量输出 | Appender 独立输出 | 支持多目标、异步扩展 |
| 自动 flush | FileAppender 每次 flush | 安全但有性能成本 |

---

## 编译和链接说明

**头文件路径**：
```
core/logger/include/logger.h
```

**库文件**：
```
core/logger/liblogger.a
```

**编译命令**：
```bash
g++-14 -std=gnu++23 -I/path/to/core/logger/include your_code.cpp \
    /path/to/core/logger/liblogger.a -o your_executable
```

**CMake 集成**（已配置）：
```cmake
target_link_libraries(your_target PRIVATE logger)
target_include_directories(your_target PRIVATE ${LOGGER_INCLUDE_DIR})
```

