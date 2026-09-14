#!/usr/bin/env bash
# mini-net 完整体验脚本（跑在 GitHub Actions 的 Remote Run 工作流里）
#   bash scripts/remote_verify.sh
set -uo pipefail

cd "${GITHUB_WORKSPACE:-$PWD}"
PORT=8080

# 关键：runner 默认 fd 上限 1024，压 1000 并发客户端会失败（还会引发客户端侧死锁）
ulimit -n 65535 2>/dev/null || true
echo "fd 上限：$(ulimit -n)"

echo "=============== 1/6 构建（Release）==============="
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null || { echo "配置失败"; exit 1; }
if ! cmake --build build -j"$(nproc)" > /tmp/build.log 2>&1; then
  echo "编译失败，错误摘要："
  grep -E "error:" /tmp/build.log | head -30
  exit 1
fi
grep -E "warning:" /tmp/build.log | head -10 || true
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
  python3 scripts/bench_quick.py --conns 500  --requests 200 --size 256 --label "threads=$threads,500conns,256B"
  echo
  python3 scripts/bench_quick.py --conns 1000 --requests 100 --size 256 --label "threads=$threads,1000conns,256B"
  kill "$pid" 2>/dev/null
  sleep 1
}
run_bench 1
run_bench 4

echo
echo "=============== 6/6 完成 ==============="
