#!/usr/bin/env bash
# 一次性装好 WSL2 Ubuntu 里的开发环境
set -euo pipefail

# 国内网络：默认的 archive.ubuntu.com 很慢，换成清华镜像（如果不在国内，删掉这一段）
if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then
  # Ubuntu 24.04+ 的新格式
  sudo sed -i 's|http://archive.ubuntu.com/ubuntu|https://mirrors.tuna.tsinghua.edu.cn/ubuntu|g; s|http://security.ubuntu.com/ubuntu|https://mirrors.tuna.tsinghua.edu.cn/ubuntu|g' /etc/apt/sources.list.d/ubuntu.sources
elif [ -f /etc/apt/sources.list ]; then
  sudo sed -i 's|http://archive.ubuntu.com/ubuntu|https://mirrors.tuna.tsinghua.edu.cn/ubuntu|g; s|http://security.ubuntu.com/ubuntu|https://mirrors.tuna.tsinghua.edu.cn/ubuntu|g' /etc/apt/sources.list
fi
echo "已切换 apt 源到清华镜像"

sudo apt update
sudo apt install -y build-essential cmake ninja-build gdb git                     clang valgrind linux-tools-common linux-tools-generic                     libgoogle-perftools-dev python3-pip netcat-openbsd

# 压测/测试用的 Python 工具链
pip3 install --user pytest pytest-xdist requests locust allure-pytest || true

echo
echo "检查："
for c in g++ cmake ninja gdb clang++ valgrind perf; do
  printf '%-10s ' "$c"; command -v "$c" >/dev/null 2>&1 && echo OK || echo "缺（apt 里换个包名再试）"
done
echo
echo "下一步：cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
