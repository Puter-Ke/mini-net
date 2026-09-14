#!/usr/bin/env bash
# 多线程对照压测（单独触发，比较慢，约 5-8 分钟）
#   bash scripts/remote_bench.sh
set -uo pipefail
cd "${GITHUB_WORKSPACE:-$PWD}"
PORT=8080
ulimit -n 65535 2>/dev/null || true
echo "fd 上限：$(ulimit -n)，CPU：$(nproc) 核"

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build -j"$(nproc)" > /dev/null || { echo "编译失败"; exit 1; }

run_case() {
  local threads="$1" conns="$2" reqs="$3" size="$4"
  ./build/mini_net_server "$PORT" 30 "$threads" > /tmp/s_$threads.log 2>&1 &
  local pid=$!
  sleep 1
  timeout 150 python3 scripts/bench_quick.py --conns "$conns" --requests "$reqs" --size "$size" \
      --label "threads=$threads,conns=$conns,size=$size" || echo "  （该场景超时）"
  kill "$pid" 2>/dev/null
  sleep 1
}

# C++ 压测客户端（如果 Claude Code 那份能编译，优先用它；否则回退到 Python）
BENCH_BIN=./build/echo_bench
if [ -x "$BENCH_BIN" ]; then
  echo "使用 C++ 压测客户端 echo_bench"
  for t in 1 4; do
    ./build/mini_net_server "$PORT" 30 "$t" > /tmp/s_$t.log 2>&1 &
    pid=$!; sleep 1
    echo "--- C++ 客户端 / IO 线程=$t ---"
    timeout 150 "$BENCH_BIN" --host 127.0.0.1 --port "$PORT" --conns 500 --requests 200 --size 256 --mode echo || echo "  （echo 模式失败）"
    kill "$pid" 2>/dev/null; sleep 1
  done
else
  echo "（未找到 echo_bench 二进制，回退 Python 客户端）"
fi

for t in 1 4; do
  run_case "$t" 500 200 256
  run_case "$t" 1000 50 256
done
echo "=============== 完成 ==============="
