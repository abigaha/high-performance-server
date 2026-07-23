#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/lib/common.sh"
cd "$ROOT"

ENV_FILE="$ROOT/.env"
DEPLOY_WAIT_TIMEOUT="${DEPLOY_WAIT_TIMEOUT:-180}"

usage() {
  cat <<EOF
用法: $(basename "$0") <子命令> [参数]

子命令:
  deploy   编译、构建镜像、启动全部服务并验证公共入口
  status   显示服务状态并检查公共入口
  health   检查 nginx 到后端的公共健康接口
  stop     停止服务并保留数据卷
  build    编译 Release 后端和前端
  image    编译并构建后端镜像
  logs [--since <时长或时间戳>]
           输出全部服务日志，或输出指定时间范围内的日志
  all      deploy 的兼容别名
  up/run   deploy 的兼容别名
  down     stop 的兼容别名
EOF
}

logs_usage() {
  cat <<EOF
用法: $(basename "$0") logs [--since <时长或时间戳>]

不带参数时输出全部历史日志。--since 的值由 Docker 解析，可使用 10m 等时长，
也可使用 Docker 支持的时间戳。
EOF
}

compose() {
  docker compose --project-directory "$ROOT" "$@"
}

# 状态、日志和停止操作不需要真实密钥；占位值保证 .env 丢失时仍可回收容器。
compose_control() {
  AUTH_SECRET=control-command-not-used \
    MYSQL_ROOT_PASSWORD=control-command-not-used \
    MYSQL_PASSWORD=control-command-not-used \
    docker compose --project-directory "$ROOT" "$@"
}

require_docker() {
  require_cmd docker || return 1
  if ! docker info >/dev/null; then
    red "错误: Docker 服务不可用"
    return 1
  fi
  if ! docker compose version >/dev/null; then
    red "错误: Docker Compose 插件不可用"
    return 1
  fi
}

require_deploy_tools() {
  require_docker || return 1
  require_cmd curl || return 1
  require_cmd openssl || return 1
  require_cmd xmake || return 1
  require_cmd npm || return 1
  require_cmd nproc || return 1
  if ! docker image inspect nginx:latest >/dev/null 2>&1; then
    red "错误: 本机不存在 nginx:latest 镜像，请先下载该镜像"
    return 1
  fi
}
env_file_value() {
  local key="$1" line value="" found=0
  while IFS= read -r line || [ -n "$line" ]; do
    line="${line%$'\r'}"
    case "$line" in
      "$key="*)
        value="${line#*=}"
        found=1
        ;;
    esac
  done < "$ENV_FILE"
  if [ "$found" -ne 1 ]; then
    return 1
  fi
  if [ "${#value}" -ge 2 ]; then
    if { [ "${value:0:1}" = '"' ] && [ "${value: -1}" = '"' ]; } ||
       { [ "${value:0:1}" = "'" ] && [ "${value: -1}" = "'" ]; }; then
      value="${value:1:${#value}-2}"
    fi
  fi
  printf '%s\n' "$value"
}

validate_env_file() {
  local auth_secret mysql_root_password mysql_user mysql_password http_port
  if ! auth_secret="$(env_file_value AUTH_SECRET)" || [ "${#auth_secret}" -lt 32 ] ||
     [[ "$auth_secret" == replace-* ]]; then
    red "错误: .env 中 AUTH_SECRET 必须是至少 32 字符的非占位密钥"
    return 1
  fi
  if ! mysql_root_password="$(env_file_value MYSQL_ROOT_PASSWORD)" ||
     [ "${#mysql_root_password}" -lt 16 ] || [[ "$mysql_root_password" == replace-* ]]; then
    red "错误: .env 中 MYSQL_ROOT_PASSWORD 必须是至少 16 字符的非占位密码"
    return 1
  fi
  if ! mysql_user="$(env_file_value MYSQL_USER)" || [ -z "$mysql_user" ] || [ "$mysql_user" = "root" ]; then
    red "错误: .env 中 MYSQL_USER 必须是非 root 用户"
    return 1
  fi
  if ! mysql_password="$(env_file_value MYSQL_PASSWORD)" || [ "${#mysql_password}" -lt 16 ] ||
     [[ "$mysql_password" == replace-* ]]; then
    red "错误: .env 中 MYSQL_PASSWORD 必须是至少 16 字符的非占位密码"
    return 1
  fi
  if [ "$mysql_password" = "$mysql_root_password" ]; then
    red "错误: MySQL 应用密码不得与 root 密码相同"
    return 1
  fi
  if ! http_port="$(env_file_value HPS_HTTP_PORT)" || [[ ! "$http_port" =~ ^[0-9]+$ ]] ||
     [ "$http_port" -lt 1 ] || [ "$http_port" -gt 65535 ]; then
    red "错误: .env 中 HPS_HTTP_PORT 必须是 1 到 65535 的整数"
    return 1
  fi
}


ensure_env_file() {
  if [ -L "$ENV_FILE" ]; then
    red "错误: $ENV_FILE 不允许是符号链接"
    return 1
  fi
  if [ -f "$ENV_FILE" ]; then
    chmod 600 "$ENV_FILE"
    validate_env_file || return 1
    return 0
  fi

  blue "=== 生成部署密钥 ==="
  local auth_secret mysql_root_password mysql_password
  auth_secret="$(openssl rand -hex 48)"
  mysql_root_password="$(openssl rand -hex 32)"
  mysql_password="$(openssl rand -hex 32)"

  umask 077
  {
    printf 'AUTH_SECRET=%s\n' "$auth_secret"
    printf 'MYSQL_ROOT_PASSWORD=%s\n' "$mysql_root_password"
    printf 'MYSQL_USER=hps\n'
    printf 'MYSQL_PASSWORD=%s\n' "$mysql_password"
    printf 'HPS_HTTP_PORT=18080\n'
  } > "$ENV_FILE"
  chmod 600 "$ENV_FILE"
  validate_env_file || return 1
  green "部署密钥已写入 $ENV_FILE（权限 0600）"
}

current_project_nginx_uses_port() {
  local port="$1" container_id state bindings binding
  if ! container_id="$(compose_control ps --quiet nginx 2>/dev/null)" || [ -z "$container_id" ]; then
    return 1
  fi
  if ! state="$(docker inspect --format '{{.State.Running}}' "$container_id" 2>/dev/null)" ||
     [ "$state" != "true" ]; then
    return 1
  fi
  if ! bindings="$(docker port "$container_id" 80/tcp 2>/dev/null)"; then
    return 1
  fi
  while IFS= read -r binding; do
    if [[ "$binding" == *":$port" ]]; then
      return 0
    fi
  done <<< "$bindings"
  return 1
}

host_port_is_listening() {
  local port="$1" port_hex file line_index local_address remote_address state ignored
  printf -v port_hex '%04X' "$port"
  for file in /proc/net/tcp /proc/net/tcp6; do
    [ -r "$file" ] || continue
    while read -r line_index local_address remote_address state ignored; do
      [ "$line_index" = "sl" ] && continue
      if [ "$state" = "0A" ] && [[ "$local_address" == *":$port_hex" ]]; then
        return 0
      fi
    done < "$file"
  done
  return 1
}

validate_http_port_availability() {
  local port container_ids container_id bindings binding container_name
  if ! port="$(env_file_value HPS_HTTP_PORT)"; then
    red "错误: 无法从 .env 读取 HPS_HTTP_PORT"
    return 1
  fi

  if current_project_nginx_uses_port "$port"; then
    green "端口预检通过: 当前项目 nginx 正在使用 127.0.0.1:$port，将原地更新"
    return 0
  fi

  if [ "$port" = "8080" ]; then
    red "错误: HPS_HTTP_PORT=8080 为 CodeQL 保留端口，请手工将 .env 设置为 HPS_HTTP_PORT=18080 后重试"
    return 1
  fi

  if ! container_ids="$(docker ps --quiet)"; then
    red "错误: 无法枚举 Docker 容器以检查端口 $port"
    return 1
  fi
  while IFS= read -r container_id; do
    [ -n "$container_id" ] || continue
    bindings="$(docker port "$container_id" 2>/dev/null || true)"
    while IFS= read -r binding; do
      if [[ "$binding" == *":$port" ]]; then
        container_name="$(docker inspect --format '{{.Name}}' "$container_id" 2>/dev/null || printf '%s' "$container_id")"
        red "错误: 端口 $port 已被 Docker 容器 ${container_name#/} 使用；8080 为 CodeQL 保留端口，建议使用 18080"
        return 1
      fi
    done <<< "$bindings"
  done <<< "$container_ids"

  if host_port_is_listening "$port"; then
    red "错误: 宿主机已有进程监听端口 $port；8080 为 CodeQL 保留端口，建议使用 18080"
    return 1
  fi

  green "端口预检通过: http://127.0.0.1:$port"
}

cmd_build() {
  blue "=== 编译 Release 后端 ==="
  xmake f -m release -y
  xmake -j"$(nproc)"

  blue "=== 构建前端 ==="
  ensure_frontend_dependencies
  (
    cd "$FRONTEND_DIR"
    npm run build
  )
  green "后端与前端构建成功"
}

build_image() {
  blue "=== 构建后端 Docker 镜像 ==="
  compose build high-performance-server
  green "后端镜像构建成功"
}

cmd_image() {
  require_deploy_tools
  ensure_env_file
  cmd_build
  compose config --quiet
  build_image
}

deployment_diagnostics() {
  red "=== 部署诊断：服务状态 ==="
  compose_control ps --all || true
  red "=== 部署诊断：完整服务日志 ==="
  compose_control logs --no-color || true
}

verify_service_health() {
  local service container_id state
  local services=(mysql high-performance-server nginx)
  for service in "${services[@]}"; do
    if ! container_id="$(compose_control ps --all --quiet "$service")" || [ -z "$container_id" ]; then
      red "服务未创建: $service"
      return 1
    fi
    if ! state="$(docker inspect --format '{{.State.Status}} {{if .State.Health}}{{.State.Health.Status}}{{else}}missing{{end}}' "$container_id")"; then
      red "无法读取服务状态: $service"
      return 1
    fi
    if [ "$state" != "running healthy" ]; then
      red "服务状态异常: $service ($state)"
      return 1
    fi
  done
}

print_backend_runtime_state() {
  local container_id runtime_state
  if ! container_id="$(compose_control ps --all --quiet high-performance-server)" || [ -z "$container_id" ]; then
    red "无法定位后端容器以读取运行时状态"
    return 1
  fi
  if ! runtime_state="$(docker inspect --format 'RestartCount={{.RestartCount}} OOMKilled={{.State.OOMKilled}}' "$container_id")"; then
    red "无法读取后端容器运行时状态"
    return 1
  fi
  printf '后端容器运行时状态: %s\n' "$runtime_state"
}

public_health_url() {
  local published port
  if ! published="$(compose_control port nginx 80)"; then
    red "错误: 无法获取 nginx 公共端口"
    return 1
  fi
  port="${published##*:}"
  if [[ ! "$port" =~ ^[0-9]+$ ]]; then
    red "错误: nginx 公共端口无效: $published"
    return 1
  fi
  printf 'http://127.0.0.1:%s/api/health\n' "$port"
}

cmd_health() {
  require_docker || return 1
  local url response
  if ! url="$(public_health_url)"; then
    return 1
  fi
  if ! response="$(curl -fsS --retry 5 --retry-delay 1 --retry-connrefused --max-time 10 "$url")"; then
    red "公共健康检查失败: $url"
    return 1
  fi
  case "$response" in
    *'"status":"ok"'*)
      green "公共健康检查通过: $url"
      printf '%s\n' "$response"
      ;;
    *)
      red "公共健康响应不符合预期: $response"
      return 1
      ;;
  esac
}

cmd_deploy() {
  require_deploy_tools
  ensure_env_file
  validate_env_file
  validate_http_port_availability
  cmd_build

  blue "=== 校验 Compose 配置 ==="
  compose config --quiet

  blue "=== 启动并等待全部服务健康 ==="
  if ! compose up --detach --build --wait --wait-timeout "$DEPLOY_WAIT_TIMEOUT" --remove-orphans; then
    deployment_diagnostics
    return 1
  fi
  if ! verify_service_health; then
    deployment_diagnostics
    return 1
  fi
  if ! cmd_health; then
    deployment_diagnostics
    return 1
  fi
  local health_url root_url
  if ! health_url="$(public_health_url)"; then
    deployment_diagnostics
    return 1
  fi
  root_url="${health_url%/api/health}"
  green "部署成功，服务入口: $root_url"
}

cmd_status() {
  require_docker || return 1
  compose_control ps --all
  local failed=0
  if ! print_backend_runtime_state; then
    failed=1
  fi
  if ! verify_service_health; then
    failed=1
  fi
  if ! cmd_health; then
    failed=1
  fi
  return "$failed"
}

cmd_stop() {
  require_docker || return 1
  blue "=== 停止服务（保留数据卷） ==="
  compose_control down --remove-orphans --timeout 30
  local remaining
  if ! remaining="$(compose_control ps --all --quiet)"; then
    red "错误: 无法验证服务是否已停止"
    return 1
  fi
  if [ -n "$remaining" ]; then
    red "错误: 停止后仍存在项目容器"
    compose_control ps --all
    return 1
  fi
  green "服务已停止，MySQL 与文件数据卷均已保留"
}

cmd_logs() {
  local since=""

  case "$#" in
    0) ;;
    1)
      if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
        logs_usage
        return 0
      fi
      red "错误: logs 只接受 --since <时长或时间戳>"
      logs_usage
      return 1
      ;;
    2)
      if [ "$1" != "--since" ]; then
        red "错误: logs 未知参数: $1"
        logs_usage
        return 1
      fi
      if [ -z "$2" ] || [[ "$2" == -* ]]; then
        red "错误: --since 需要有效的 Docker 时长或时间戳"
        logs_usage
        return 1
      fi
      since="$2"
      ;;
    *)
      red "错误: logs 参数过多"
      logs_usage
      return 1
      ;;
  esac

  require_docker || return 1
  if [ -n "$since" ]; then
    compose_control logs --no-color --since "$since"
  else
    compose_control logs --no-color
  fi
}

handle_menu_choice() {
  case "$1" in
    1) cmd_deploy ;;
    2) cmd_status ;;
    3) cmd_health ;;
    4) cmd_stop ;;
    5) cmd_build ;;
    6) cmd_logs ;;
    *) red "无效选择" ;;
  esac
}

menu() {
  local items=(
    "deploy:一键部署"
    "status:查看状态"
    "health:健康检查"
    "stop:停止服务"
    "build:构建后端与前端"
    "logs:查看完整日志"
  )
  menu_loop "Docker 部署工具（$ROOT）" "${items[@]}"
}

if [ $# -gt 0 ]; then
  case "$1" in
    deploy|all|up|run) cmd_deploy ;;
    status) cmd_status ;;
    health) cmd_health ;;
    stop|down) cmd_stop ;;
    build) cmd_build ;;
    image) cmd_image ;;
    logs)
      shift
      cmd_logs "$@"
      ;;
    -h|--help) usage ;;
    *) red "未知子命令: $1"; usage; exit 1 ;;
  esac
else
  menu
fi
