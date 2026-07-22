# Docker 部署说明

> 本文描述仓库当前的 `Dockerfile`、`docker-compose.yml`、`deploy/nginx.conf` 和
> `scripts/docker.sh`，取代早期尚未落地的镜像与端口设想。

## 当前架构

```text
浏览器
  -> http://127.0.0.1:18080
  -> nginx:80（静态前端、SPA 回退、/api 与 /ws 反向代理）
  -> high-performance-server:9090
  -> MySQL 8.0
```

- nginx 是唯一公共入口，默认只绑定宿主机回环地址 `127.0.0.1`。
- 后端 `9090` 只通过 Compose 内部网络暴露，不直接发布到宿主机。
- `http://localhost:8080` 是 CodeQL 服务地址，不是本应用入口；部署脚本会拒绝把应用端口配置为 `8080`。
- nginx 的 `client_max_body_size` 为 `110m`，高于后端 VIP 默认上传上限 `100 MiB`，最终权限和大小判断仍以后端为准。

## 镜像与服务

### 后端镜像

当前 `Dockerfile` 是运行时镜像，不在容器内编译源码：

- 基础镜像为 `ubuntu:24.04`；
- 仅安装 `libssl3t64`、`libstdc++6` 和健康检查所需的 `curl`；
- `scripts/docker.sh` 先在宿主机以 Release 模式构建，再把 `bin/` 和 `lib/` 中的运行产物复制进镜像；
- 容器使用非特权用户 `hps`，工作目录为 `/app`；
- `/app/data` 由 `app_data` 卷持久化；
- 容器健康检查访问 `http://127.0.0.1:9090/api/health`。

`.dockerignore` 会排除源码构建目录、测试、文档、脚本和整个 `frontend/`。前端由宿主机构建，生成的 `frontend/dist` 以只读卷挂载到 nginx，不进入后端镜像。

### Compose 服务

| 服务 | 作用 | 持久化与可达性 |
|------|------|----------------|
| `mysql` | MySQL 8.0，首次启动执行 `db/schema.sql` | `mysql_data` 卷，仅 Compose 内部可达 |
| `high-performance-server` | C++ 后端，镜像名 `hps-server:local` | `app_data` 卷，内部端口 `9090` |
| `nginx` | 前端静态资源、SPA 回退、API/WebSocket 代理 | 默认发布 `127.0.0.1:18080:80` |

nginx 使用本机已有的 `nginx:latest` 镜像，Compose 设置了 `pull_policy: never`。部署前若本机没有该镜像，`scripts/docker.sh` 会明确失败，不会隐式拉取。

## 部署配置

`scripts/docker.sh deploy` 在根目录缺少 `.env` 时自动生成权限为 `0600` 的文件，内容包括：

- 至少 32 字符的 `AUTH_SECRET`；
- MySQL root 密码和独立的应用用户密码；
- 默认应用用户 `hps`；
- 默认公共端口 `HPS_HTTP_PORT=18080`。

已有 `.env` 会在部署前校验。应用密码不得与 root 密码相同，`MYSQL_USER` 不得为 `root`，公共端口必须是 `1` 到 `65535` 的整数。需要自定义端口时修改 `.env` 中的 `HPS_HTTP_PORT`；不要使用为 CodeQL 保留的 `8080`。

## 使用流程

部署需要 Docker 与 Compose 插件、`curl`、`openssl`、`xmake`、`npm`、`nproc`，并需要本机已有 `nginx:latest` 镜像。

```bash
# 构建 Release 后端和前端、构建后端镜像、启动并等待所有服务健康
bash scripts/docker.sh deploy

# 查看 Compose 状态并重新检查公共健康接口
bash scripts/docker.sh status

# 仅检查 nginx 公共入口到后端的健康链路
bash scripts/docker.sh health

# 完整输出三个服务的全部历史日志
bash scripts/docker.sh logs

# 仅输出最近 10 分钟的日志
bash scripts/docker.sh logs --since 10m

# 停止并移除容器，保留 mysql_data 和 app_data 数据卷
bash scripts/docker.sh stop
```

`logs --since` 的值可以是 Docker 支持的时长（如 `10m`）或时间戳。脚本将该值原样交给 Docker 解析，不对日志做行级过滤；省略 `--since` 时继续输出全部历史日志。

部署成功后的默认入口为：

```text
http://127.0.0.1:18080
```

健康接口为 `http://127.0.0.1:18080/api/health`。`deploy` 会校验 Compose 配置、等待 `mysql`、`high-performance-server` 和 `nginx` 全部进入 `running healthy`，再通过公共入口检查响应中是否包含 `"status":"ok"`；失败时会输出完整服务状态和日志。

`build` 只构建宿主机 Release 后端和前端，`image` 在此基础上构建后端镜像；日常部署优先使用 `deploy`，避免遗漏健康检查。

## 验收边界

`scripts/docker.sh health` 验证的是容器状态和公共健康链路，不代替注册、登录、上传及响应式界面的浏览器验收。真实用户流程使用 `frontend/tests/e2e/deployment.spec.ts`，执行方式见 `plan/end-to-end-verification.md`。

本文只说明当前可执行入口，不记录某一次部署是否通过；本轮真实结果应在实际运行相关命令后再回填到对应验收记录。
