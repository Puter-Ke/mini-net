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
echo "=============== 5/5 空闲连接超时验证（服务端设 2 秒超时）==============="
./build/mini_net_server 8081 2 > /tmp/server_idle.log 2>&1 &
IDLE_PID=$!
sleep 1
# 连上但一个字节都不发，模拟"僵尸连接"
for i in 1 2 3; do timeout 6 bash -c 'exec 3<>/dev/tcp/127.0.0.1/8081; sleep 5' >/dev/null 2>&1 & done
sleep 5
echo "服务端日志里的空闲清理记录："
grep -E "空闲超时|本轮清理" /tmp/server_idle.log | head -6 || echo "  （没找到清理记录 —— 说明 M2 没生效）"
echo "清理后在线连接数："
grep -oE "在线 [0-9]+" /tmp/server_idle.log | tail -1
kill "$IDLE_PID" 2>/dev/null

echo
echo "=============== 完成 ==============="

