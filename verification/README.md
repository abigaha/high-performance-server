# 功能验证报告

## 概述

本文档定义了 High-Performance Server 项目的完整功能验证流程。验证分为 15 个维度，覆盖编译、单元测试、REST API、错误处理、Keep-Alive、WebSocket、信号停止、SSL/TLS、配置参数、文件系统、文件上传下载、并发连接、边界条件等所有核心功能。

## 前置条件

```bash
# 安装依赖
sudo apt install -y build-essential libssl-dev libstdc++-11-dev curl
npm install -g wscat        # WebSocket 客户端（可选）
xmake require                # 安装 gtest / nlohmann_json

# 创建数据目录
mkdir -p data

# SSL 证书（如不存在）
ls build/certs/cert.pem 2>/dev/null || openssl req -x509 -newkey rsa:2048 \
  -keyout build/certs/key.pem -out build/certs/cert.pem -days 365 -nodes \
  -subj "/CN=localhost"
```

## 验证流程

### V1：编译与静态分析

```bash
# Release 编译
xmake f -m release -y && xmake -j$(nproc)

# 全量 Lint
bash scripts/lint.sh -j $(nproc)
```

**门禁**：0 error + 0 warning / clang-tidy 0/0/0 + cppcheck 0/0/0/0

---

### V2：单元测试

```bash
bash scripts/test.sh
```

**门禁**：20 个测试二进制全部 passed

---

### V3：服务器启动

```bash
# 修改配置文件端口为 9090（config.json → "port": 9090）
bin/high-performance-server &
PID=$!
sleep 2
netstat -tlnp | grep 9090
```

**预期**：`LISTEN` 状态，PID 匹配

---

### V4：健康检查

```bash
curl -s -o /dev/null -w "%{http_code}" http://localhost:9090/api/health
# 预期: 200

curl -s http://localhost:9090/api/health
# 预期: {"status":"ok","uptime":<正整数>}
```

---

### V5：REST API

```bash
# GET /api/users/:id
curl -s -w " HTTP_%{http_code}" http://localhost:9090/api/users/1
# 预期: {"user_id":1,"username":"mock_user"} HTTP_200

# POST /api/users
curl -s -X POST -w " HTTP_%{http_code}" http://localhost:9090/api/users
# 预期: {"status":"created"} HTTP_201

# GET /api/users/:id/history
curl -s -w " HTTP_%{http_code}" http://localhost:9090/api/users/1/history
# 预期: {"downloads":[]} HTTP_200

# GET /api/files/:hash
curl -s -w " HTTP_%{http_code}" http://localhost:9090/api/files/abc123
# 预期: {"error":"file not found"} HTTP_404

# Content-Type 验证
curl -s -D - http://localhost:9090/api/health | grep -i content-type
# 预期: content-type: application/json
```

---

### V6：HTTP 错误处理

```bash
# 404 未注册路由
curl -s http://localhost:9090/api/nonexistent
# 预期: 404 Not Found: /api/nonexistent

# 413 请求体过大
dd if=/dev/zero bs=1M count=105 2>/dev/null | \
  curl -s -X POST --data-binary @- http://localhost:9090/api/users
# 预期: 413 Payload Too Large: 请求体过大
```

---

### V7：Keep-Alive

```bash
python3 -c "
import socket
s = socket.socket()
s.connect(('localhost', 9090))
s.settimeout(3)
req = b'GET /api/health HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n'
for i in range(10):
    s.sendall(req)
    resp = b''
    while True:
        chunk = s.recv(4096)
        if not chunk: break
        resp += chunk
        if b'\r\n\r\n' in resp:
            h = resp.split(b'\r\n\r\n')[0]
            status = h.split(b'\r\n')[0].decode()
            print(f'Request {i+1}: {status}')
            resp = b''
            break
s.close()
"
```

**预期**：10 个请求均返回 `HTTP/1.1 200 OK`

---

### V8：WebSocket

```bash
python3 verification/ws_test.py
```

**预期**：
- 握手返回 101 Switching Protocols
- TEXT 帧被服务器接收

**注意**：升级后的帧通信存在已知架构限制（epoll 双读竞争），帧编解码功能在单元测试中覆盖。

---

### V9：信号停止

```bash
kill -2 $PID    # 发送 SIGINT
sleep 2
tail -5 server.log
```

**预期日志**：
```
收到信号 2，正在关闭服务器...
TcpServer 正在停止...
TcpServer 已停止
服务器已停止
```

---

### V10：SSL/TLS

```bash
# 修改 config.json → "ssl.enabled": true, "cert_file": "./build/certs/cert.pem", "key_file": "./build/certs/key.pem"
bin/high-performance-server &
sleep 2
grep 'SSL 已启用' server.log

# HTTPS 请求
curl -sk https://localhost:9090/api/health
# 预期: {"status":"ok","uptime":N}

# 证书验证
curl --cacert build/certs/cert.pem https://localhost:9090/api/health
# 预期: 200

# 双模式：明文 HTTP 同端口
curl -s http://localhost:9090/api/health
# 预期: 200
```

---

### V11：命令行参数

```bash
bin/high-performance-server --port 9091 &
sleep 2
netstat -tlnp | grep 9091
kill %1
```

**注意**：config.json 会覆盖命令行参数（JSON 后加载）。需要时请修改 config.json。

---

### V12：文件上传/下载/哈希

覆盖文件全生命周期：上传 → 元信息 → 下载比对 → 二进制 → 大文件 → 重复上传幂等。

```bash
# 上传 100B 文件
TMP_DATA=$(mktemp)
python3 -c "import sys; sys.stdout.buffer.write(b'x' * 100)" > "$TMP_DATA"
V12_HASH=$(sha256sum "$TMP_DATA" | cut -d' ' -f1)
curl -s -X POST --data-binary @"$TMP_DATA" http://localhost:9090/api/files/upload
# 预期: 201 + {"hash":"<sha256>","size":100}

# 查元信息
curl -s http://localhost:9090/api/files/$V12_HASH
# 预期: 200 + {"file_hash":"...","file_size":100,...}

# 下载比对 hash
curl -s -o /tmp/dl http://localhost:9090/api/files/$V12_HASH/download
sha256sum /tmp/dl  # 预期: 与 V12_HASH 一致

# 上传含 null 二进制文件（256 字节）
python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256)))" > "$TMP_DATA"
V12_HASH2=$(sha256sum "$TMP_DATA" | cut -d' ' -f1)
curl -s -X POST --data-binary @"$TMP_DATA" http://localhost:9090/api/files/upload
# 预期: 201

# 上传 5MB 大文件
python3 -c "import sys; sys.stdout.buffer.write(b'ABCD' * (5*1024*1024//4))" > "$TMP_DATA"
V12_HASH3=$(sha256sum "$TMP_DATA" | cut -d' ' -f1)
curl -s --max-time 30 -X POST --data-binary @"$TMP_DATA" http://localhost:9090/api/files/upload
# 预期: 201

# 下载不存在文件
curl -s http://localhost:9090/api/files/000.../download
# 预期: 404

# 重复上传（幂等）
curl -s -X POST -d 'xxxxxxxxxx...' http://localhost:9090/api/files/upload
# 预期: 200 + {"exists":true}
```

---

### V13：帮助信息

```bash
bin/high-performance-server --help
```

**预期**：列出全部 11 个选项

---

### V14：并发连接

4 个线程同时发起 HTTP 请求，验证服务器并发处理能力。

```bash
python3 -c "
import socket, concurrent.futures
def req():
    s = socket.socket(); s.settimeout(10)
    s.connect(('localhost', 9090))
    s.sendall(b'GET /api/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
    resp = b''
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk: break
            resp += chunk
    except socket.timeout: pass
    s.close()
    return b'200 OK' in resp
with concurrent.futures.ThreadPoolExecutor(4) as ex:
    print(sum(1 for r in ex.map(lambda _: req(), range(4)) if r))
"
```

**预期**：输出 `4`，表示全部请求成功。

---

### V15：边界条件

```bash
# 空 body POST
curl -s -X POST -d '' http://localhost:9090/api/users
# 预期: 非 000（500 / 405 等均可）

# PUT/DELETE 到 GET-only 路由
curl -s -X PUT http://localhost:9090/api/health
# 预期: 405 或 404（非 200）
curl -s -X DELETE http://localhost:9090/api/health
# 预期: 405 或 404（非 200）
```

```bash
bin/high-performance-server --help
```

**预期**：列出全部 11 个选项

---

## 已知问题

| # | 问题 | 严重度 | 说明 |
|---|------|--------|------|
| B1 | WebSocket 升级后 epoll 与 WS 事件循环双读竞争 | 高 | WS 帧数据被 epoll 的 `handle_read` 读取后重新入队，WS 帧无法到达 `WsConnection`。帧编解码由单元测试覆盖 |
| B2 | config.json 覆盖命令行参数 | 低 | ✅ 已修复：`parse_json_file` 先于 `parse_cmd_args`，CLI 参数覆盖 JSON |
| B3 | DELETE/PUT 等方法返回 404 而非 405 | 低 | ✅ 已修复：路由存在但方法不匹配时返回 405 |

## 快速运行

```bash
bash verification/verify.sh
```

一键执行全部验证流程，输出 PASS/FAIL 报告。
