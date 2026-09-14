#!/usr/bin/env bash
# M1 远程验证：构建 → 单测 → 冒烟 → 快速压测 → 收工
# 在 GitHub Actions 的 Remote Run 工作流里调用：bash scripts/remote_m1_bench.sh
set -uo pipefail

cd "${GITHUB_WORKSPACE:-$PWD}"
PORT=8080

echo "=============== 1/4 构建（Release）==============="
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null || { echo "配置失败"; exit 1; }
cmake --build build -j"$(nproc)" > /dev/null || { echo "编译失败"; exit 1; }
echo "构建成功：$(ls -la build/mini_net_server | awk '{print $5" 字节"}')"

echo
echo "=============== 2/4 单元测试 ==============="
ctest --test-dir build --output-on-failure 2>&1 | tail -6

echo
echo "=============== 3/4 冒烟测试（回显 + 1000 并发 + fd 泄漏）==============="
CONNS=1000 bash scripts/smoke_test.sh "$PORT" 2>&1 | grep -E "\[通过\]|\[失败\]|fd |结果|通过 [0-9]+ 项"

echo
echo "=============== 4/4 快速压测（服务端 + 客户端同机，仅作基线）==============="
./build/mini_net_server "$PORT" > /tmp/server.log 2>&1 &
SERVER_PID=$!
sleep 1

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "服务未能启动，日志："; cat /tmp/server.log; exit 1
fi
echo "服务已启动 (PID=$SERVER_PID)，环境：$(nproc) 核 / $(free -h | awk '/Mem:/{print $2}') 内存"
echo
python3 scripts/bench_quick.py --conns 100 --requests 200 --size 64  --label "100-conns-64B"
echo
python3 scripts/bench_quick.py --conns 500 --requests 100 --size 64  --label "500-conns-64B"

kill "$SERVER_PID" 2>/dev/null
echo
echo "=============== 完成 ==============="
