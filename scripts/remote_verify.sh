#!/usr/bin/env bash
# mini-net 完整体验脚本（跑在 GitHub Actions 的 Remote Run 工作流里）
#   bash scripts/remote_verify.sh
set -uo pipefail

cd "${GITHUB_WORKSPACE:-$PWD}"
PORT=8080

echo "=============== 1/6 构建（Release）==============="
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null || { echo "配置失败"; exit 1; }
cmake --build build -j"$(nproc)" 2>&1 | tail -5 || { echo "编译失败"; exit 1; }
echo "构建成功，二进制大小：$(stat -c %s build/mini_net_server) 字节"

echo
echo "=============== 2/6 单元/集成测试 ==============="
ctest --test-dir build --output-on-failure 2>&1 | tail -12

echo
echo "=============== 3/6 冒烟测试（回显 + 1000 并发 + fd 泄漏）==============="
CONNS=1000 bash scripts/smoke_test.sh "$PORT" 2>&1 | grep -E "\[通过\]|\[失败\]|fd |通过 [0-9]+ 项"

echo
echo "=============== 4/6 空闲连接超时（服务端设 2 秒）==============="
./build/mini_net_server 8081 2 2 > /tmp/server_idle.log 2>&1 &
IDLE_PID=$!
sleep 1
for i in 1 2 3; do timeout 6 bash -c 'exec 3<>/dev/tcp/127.0.0.1/8081; sleep 5' >/dev/null 2>&1 & done
sleep 5
grep -E "空闲超时|本轮清理" /tmp/server_idle.log | head -6 || echo "  （没有清理记录，M2 异常）"
kill "$IDLE_PID" 2>/dev/null

echo
echo "=============== 5/6 性能对照：单线程 vs 4 线程 ==============="
run_bench() {
  local threads="$1"
  ./build/mini_net_server "$PORT" 30 "$threads" > /tmp/server_$threads.log 2>&1 &
  local pid=$!
  sleep 1
  echo "--- IO 线程数 = $threads ---"
  python3 scripts/bench_quick.py --conns 100 --requests 300 --size 64 --label "threads=$threads,100conns"
  echo
  python3 scripts/bench_quick.py --conns 500 --requests 100 --size 64 --label "threads=$threads,500conns"
  kill "$pid" 2>/dev/null
  sleep 1
}
run_bench 1
run_bench 4

echo
echo "=============== 6/6 完成 ==============="
