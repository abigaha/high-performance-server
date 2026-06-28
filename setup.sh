#!/bin/bash
# Linux 开发环境安装脚本
# 在远程服务器上运行：bash setup.sh

set -e

echo "=== 安装 C++ 编译工具链 ==="
sudo apt-get update
sudo apt-get install -y g++ gdb make

echo "=== 安装 xmake ==="
curl -fsSL https://xmake.io/shget.text | bash
source ~/.xmake/profile

echo "=== 安装 clang-tidy + cppcheck ==="
sudo apt-get install -y clang-tidy cppcheck clang-format

echo "=== 安装 CodeQL CLI（用于宿主机预下载查询包）==="
wget https://github.com/github/codeql-cli-binaries/releases/download/v2.21.0/codeql-linux64.zip
sudo unzip codeql-linux64.zip -d /opt/
sudo ln -sf /opt/codeql/codeql /usr/local/bin/codeql
rm codeql-linux64.zip

echo "=== 预下载 CodeQL C++ 查询包 ==="
codeql pack download codeql/cpp-queries
codeql pack download codeql/cpp-all

echo "=== 设置环境变量 ==="
echo 'export CODEQL_SERVER_URL=http://localhost:8080' >> ~/.bashrc

echo ""
echo "=========================================="
echo "安装完成！请执行以下操作："
echo "1. source ~/.bashrc"
echo "2. 确认 CodeQL 容器在运行：docker ps | grep codeql-server"
echo "3. 启动 opencode 开始开发"
echo "=========================================="
