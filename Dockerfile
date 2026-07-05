# ===== Stage 1: Builder =====
FROM ubuntu:22.04 AS builder

LABEL stage=builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    curl \
    ca-certificates \
    libssl-dev \
    libstdc++-11-dev \
    && rm -rf /var/lib/apt/lists/*

# 安装 xmake
RUN curl -fsSL https://xmake.io/shget.text | bash

ENV XMAKE_ROOT=y \
    PATH=/root/.local/bin:$PATH

WORKDIR /src

# 先拷贝构建配置以利用 docker 缓存
COPY xmake.lua ./
COPY core/xmake.lua core/
COPY logger/xmake.lua logger/
COPY memory-pool/xmake.lua memory-pool/
COPY file-system/xmake.lua file-system/
COPY db/xmake.lua db/
COPY net/xmake.lua net/
COPY net/tcp/xmake.lua net/tcp/
COPY net/tcp/tcp_client/xmake.lua net/tcp/tcp_client/
COPY net/tcp/tcp_server/xmake.lua net/tcp/tcp_server/
COPY net/coroutine/xmake.lua net/coroutine/
COPY net/thread-pool/xmake.lua net/thread-pool/
COPY net/http/xmake.lua net/http/
COPY net/ssl/xmake.lua net/ssl/
COPY net/websocket/xmake.lua net/websocket/
COPY net/file-transfer/xmake.lua net/file-transfer/

# 安装依赖（gtest, nlohmann_json）
RUN xmake require

# 拷贝源码
COPY core/ core/
COPY logger/ logger/
COPY memory-pool/ memory-pool/
COPY file-system/ file-system/
COPY db/ db/
COPY net/ net/

# 编译 release 版本
RUN xmake f -m release -y && \
    xmake -j$(nproc)

# ===== Stage 2: Runtime =====
FROM ubuntu:22.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

# 创建数据目录
RUN mkdir -p /data

WORKDIR /app

# 从 builder 拷贝产物
COPY --from=builder /src/build/linux/x86_64/release/high-performance-server .
COPY --from=builder /src/lib/ lib/
COPY --from=builder /src/config.json .
COPY --from=builder /src/data/ data/ 2>/dev/null || true

# SSL 证书（若存在）
COPY --from=builder /src/build/cert.pem ./build/cert.pem 2>/dev/null || true
COPY --from=builder /src/build/key.pem ./build/key.pem 2>/dev/null || true

EXPOSE 9000

# 默认启动命令（可通过 docker run 覆盖）
CMD ["./high-performance-server", "--config", "config.json"]
