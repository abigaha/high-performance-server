#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

red()    { printf "\033[31m%s\033[0m\n" "$*"; }
green()  { printf "\033[32m%s\033[0m\n" "$*"; }
yellow() { printf "\033[33m%s\033[0m\n" "$*"; }
blue()   { printf "\033[34m%s\033[0m\n" "$*"; }

cmd_build() {
  blue "=== 编译 Release ==="
  xmake f -m release -y && xmake -j"$(nproc)"
  green "编译成功"
}

cmd_image() {
  blue "=== 构建 Docker 镜像 ==="
  docker build -t hps-server .
  local size; size=$(docker images hps-server --format "{{.Size}}")
  green "镜像构建成功，大小: $size"
}

cmd_run() {
  blue "=== 运行容器 ==="
  docker run -d --name hps-server -p 9090:9090 hps-server
  sleep 1
  local rc; rc=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:9090/api/health 2>/dev/null || echo "000")
  if [ "$rc" = "200" ]; then
    green "容器运行正常，HTTP $rc"
  else
    red "健康检查失败，HTTP $rc"
    docker logs hps-server
    exit 1
  fi
}

cmd_stop() {
  blue "=== 停止容器 ==="
  docker stop hps-server 2>/dev/null || true
  docker rm hps-server 2>/dev/null || true
  green "容器已停止"
}

cmd_compose_up() {
  blue "=== docker-compose 启动 ==="
  docker compose up -d
  green "服务已启动"
}

cmd_compose_down() {
  blue "=== docker-compose 停止 ==="
  docker compose down
  green "服务已停止"
}

cmd_all() {
  cmd_build
  cmd_image
  cmd_stop 2>/dev/null || true
  cmd_run
}

menu() {
  while true; do
    echo
    blue "===== Docker 工具（$ROOT）====="
    echo "1)  编译 Release"
    echo "2)  构建 Docker 镜像"
    echo "3)  运行容器（端口 9090）"
    echo "4)  停止容器"
    echo "5)  docker-compose 启动"
    echo "6)  docker-compose 停止"
    echo "7)  全流程（编译 → 镜像 → 运行）"
    echo "q)  退出"
    printf "选择: "
    read -r choice
    echo
    case "$choice" in
      1) cmd_build ;;
      2) cmd_image ;;
      3) cmd_run ;;
      4) cmd_stop ;;
      5) cmd_compose_up ;;
      6) cmd_compose_down ;;
      7) cmd_all ;;
      q|Q) exit 0 ;;
      *) red "无效选择" ;;
    esac
  done
}

if [ $# -gt 0 ]; then
  case "$1" in
    build) cmd_build ;;
    image) cmd_image ;;
    run) cmd_run ;;
    stop) cmd_stop ;;
    up) cmd_compose_up ;;
    down) cmd_compose_down ;;
    all) cmd_all ;;
    *) echo "用法: $0 {build|image|run|stop|up|down|all}"; exit 1 ;;
  esac
else
  menu
fi
