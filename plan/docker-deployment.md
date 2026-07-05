# Docker 化部署计划

## 目标
- 多阶段构建，分离构建/运行环境
- 支持 docker-compose 编排
- 支持 SSL 证书和配置文件挂载
- 镜像体积 ≤ 200MB

## 步骤

### 1. Dockerfile 多阶段构建
- **Builder 阶段**：ubuntu:22.04, 安装 build-essential + libssl + xmake, 编译 release
- **Runtime 阶段**：ubuntu:22.04, 仅 libssl + libstdc++, 拷贝二进制/动态库/config

### 2. .dockerignore
- 排除 build/ .xmake/ .git/ tests/ scripts/ 等

### 3. docker-compose.yml
- 服务: high-performance-server
- 数据库: mysql:8.0
- 卷挂载: config.json, SSL 证书, 数据目录

### 4. 验证
- `docker compose up -d` 启动
- `curl http://localhost:9000/api/health` 返回 200
- `docker compose down` 优雅停止

### 5. CI 集成
- GitHub Actions 构建镜像
- 推送至 Docker Registry
