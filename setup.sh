#!/usr/bin/env bash
# Ubuntu/Debian 基础开发环境初始化脚本。

set -euo pipefail

readonly PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
temp_dir=""

show_usage() {
  printf '%s\n' \
    "用法：bash setup.sh" \
    "" \
    "安装项目的基础编译、调试、静态分析和前端工具，并拉取 xmake 包依赖。" \
    "Docker、wrk 和 Google Benchmark 属于可选能力，不会自动安装。"
}

cleanup() {
  if [[ -n "$temp_dir" && -d "$temp_dir" ]]; then
    rm -rf -- "$temp_dir"
  fi
}

fail() {
  printf '错误：%s\n' "$*" >&2
  exit 1
}

run_as_root() {
  if ((EUID == 0)); then
    "$@"
  else
    sudo "$@"
  fi
}

run_apt() {
  run_as_root env DEBIAN_FRONTEND=noninteractive apt-get "$@"
}

node_version_supported() {
  command -v node >/dev/null 2>&1 || return 1
  node -e '
    const [major, minor] = process.versions.node.split(".").map(Number);
    const supported = (major === 20 && minor >= 19) ||
      (major === 22 && minor >= 12) || major > 22;
    process.exit(supported ? 0 : 1);
  '
}

xmake_version_supported() {
  command -v xmake >/dev/null 2>&1 || return 1
  local version_output
  version_output="$(xmake --version 2>/dev/null)" || return 1
  [[ "$version_output" =~ xmake[[:space:]]+v([0-9]+) ]] &&
    ((10#${BASH_REMATCH[1]} >= 3))
}

install_nodejs() {
  if node_version_supported && command -v npm >/dev/null 2>&1; then
    printf '复用已安装的 Node.js %s 和 npm %s。\n' "$(node --version)" "$(npm --version)"
    return
  fi

  printf '%s\n' "=== 安装满足前端要求的 Node.js 22 和 npm ==="
  temp_dir="$(mktemp -d)"
  curl -fsSL https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key \
    -o "$temp_dir/nodesource.key"
  gpg --batch --yes --dearmor \
    --output "$temp_dir/nodesource.gpg" "$temp_dir/nodesource.key"

  run_as_root install -d -m 0755 /etc/apt/keyrings
  run_as_root install -m 0644 "$temp_dir/nodesource.gpg" /etc/apt/keyrings/nodesource.gpg
  printf 'deb [arch=%s signed-by=/etc/apt/keyrings/nodesource.gpg] https://deb.nodesource.com/node_22.x nodistro main\n' \
    "$(dpkg --print-architecture)" >"$temp_dir/nodesource.list"
  run_as_root install -m 0644 "$temp_dir/nodesource.list" \
    /etc/apt/sources.list.d/nodesource.list

  run_apt update
  run_apt install -y nodejs

  node_version_supported || fail "Node.js 版本不满足前端要求，需要 20.19+ 或 22.12+。"
  command -v npm >/dev/null 2>&1 || fail "Node.js 已安装，但未找到 npm。"
}

install_xmake() {
  if xmake_version_supported; then
    printf '复用已安装的 xmake：%s\n' "$(command -v xmake)"
    return
  fi

  if command -v xmake >/dev/null 2>&1; then
    printf '%s\n' "检测到旧版 xmake，将通过官方安装器升级到 v3。"
  fi
  printf '%s\n' "=== 安装 xmake ==="
  curl -fsSL https://xmake.io/shget.text | bash
  export PATH="${HOME}/.local/bin:${HOME}/.xmake/bin:${PATH}"
  if ! command -v xmake >/dev/null 2>&1 && [[ -r "${HOME}/.xmake/profile" ]]; then
    # 官方安装器通过此文件补充当前 Shell 的搜索路径。
    source "${HOME}/.xmake/profile"
  fi
  xmake_version_supported || fail "未找到可用的 xmake v3，请检查安装器输出和 PATH。"
}

if (($# > 0)); then
  case "$1" in
    -h | --help)
      show_usage
      exit 0
      ;;
    *)
      show_usage >&2
      fail "不支持参数：$1"
      ;;
  esac
fi

trap cleanup EXIT

[[ -r /etc/os-release ]] || fail "无法识别操作系统，仅支持 Ubuntu/Debian。"
# shellcheck disable=SC1091
source /etc/os-release
if [[ " ${ID:-} ${ID_LIKE:-} " != *" ubuntu "* && \
      " ${ID:-} ${ID_LIKE:-} " != *" debian "* ]]; then
  fail "当前系统 ${PRETTY_NAME:-未知} 不受支持，仅支持 Ubuntu/Debian。"
fi

if ((EUID != 0)) && ! command -v sudo >/dev/null 2>&1; then
  fail "安装系统依赖需要 root 权限或 sudo。"
fi

printf '=== 初始化 %s 开发环境 ===\n' "${PRETTY_NAME:-Ubuntu/Debian}"
run_apt update
run_apt install -y \
  bc \
  build-essential \
  ca-certificates \
  clang-format \
  clang-tidy \
  cppcheck \
  curl \
  g++ \
  gdb \
  git \
  gnupg \
  libboost-system-dev \
  libssl-dev \
  make \
  openssl \
  pkg-config \
  python3 \
  tar \
  unzip

if [[ ! -f /usr/include/boost/mysql.hpp ]]; then
  printf '%s\n' \
    "警告：当前发行版的 Boost 不含 boost/mysql.hpp。" \
    "数据库模块要求 Boost 1.82+；请升级系统 Boost 后再编译完整项目。" >&2
fi

install_nodejs
install_xmake

[[ -f "$PROJECT_ROOT/xmake.lua" ]] || fail "项目根目录缺少 xmake.lua。"
printf '%s\n' "=== 拉取 xmake 项目依赖 ==="
if ((EUID == 0)); then
  (cd "$PROJECT_ROOT" && xmake require --root -y)
else
  (cd "$PROJECT_ROOT" && xmake require -y)
fi

printf '%s\n' \
  "" \
  "基础开发环境初始化完成。" \
  "Node.js：$(node --version)，npm：$(npm --version)" \
  "xmake：$(command -v xmake)" \
  "" \
  "以下能力按需安装，本脚本不会自动修改容器或压测环境：" \
  "- Docker 部署：按 Docker 官方文档安装 Docker Engine 和 Compose 插件。" \
  "- HTTP RPS 压测：sudo apt-get install wrk" \
  "- Google Benchmark 微基准：sudo apt-get install libbenchmark-dev"
