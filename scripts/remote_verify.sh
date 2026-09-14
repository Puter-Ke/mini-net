#!/usr/bin/env bash
# 快速正确性验证（目标：3 分钟内跑完，绝不挂住 CI）
#   bash scripts/remote_verify.sh
set -uo pipefail

cd "${GITHUB_WORKSPACE:-$PWD}"
PORT=8080
ulimit -n 65535 2>/dev/null || true

echo "=============== 1/5 构建（Release）==============="
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null || { echo "配置失败"; exit 1; }
if ! cmake --build build -j"$(nproc)" > /tmp/build.log 2>&1; then
  echo "编译失败，错误摘要："
  grep -E "error:" /tmp/build.log | head -20
  exit 1
fi
echo "构建成功：$(stat -c %s build/mini_net_server) 字节（服务端）"
grep -E "warning:" /tmp/build.log | head -5 || true

echo
echo "=============== 2/5 单元/集成测试 ==============="
timeout 300 ctest --test-dir build --timeout 30 --output-on-failure > /tmp/ctest.log 2>&1
CTEST_RC=$?
echo "ctest 返回码: $CTEST_RC"
grep -E "^ *[0-9]+/[0-9]+ Test" /tmp/ctest.log | tail -3
if [ "$CTEST_RC" != "0" ]; then
  echo "--- 失败的测试 ---"
  grep -A3 "The following tests FAILED" /tmp/ctest.log
  echo "--- 断言详情 ---"
  grep -E "Failure|Expected|Which is|Actual|Value of|error:|Segmentation" /tmp/ctest.log | head -25
fi

echo
echo "=============== 3/5 冒烟测试（1000 并发 + fd 泄漏）==============="
timeout 180 env CONNS=1000 bash scripts/smoke_test.sh "$PORT" 2>&1 | grep -E "\[通过\]|\[失败\]|fd |通过 [0-9]+ 项"

echo
echo "=============== 4/5 空闲连接超时 ==============="
./build/mini_net_server 8081 2 2 > /tmp/server_idle.log 2>&1 &
IDLE_PID=$!
sleep 1
for i in 1 2 3; do timeout 6 bash -c 'exec 3<>/dev/tcp/127.0.0.1/8081; sleep 5' >/dev/null 2>&1 & done
sleep 5
grep -E "空闲超时|本轮清理" /tmp/server_idle.log | head -5 || echo "  （无清理记录，异常）"
kill "$IDLE_PID" 2>/dev/null

echo
echo "=============== 5/5 轻量压测（单线程，500 连接，仅作基线）==============="
./build/mini_net_server "$PORT" 30 1 > /tmp/s1.log 2>&1 &
P1=$!
sleep 1
timeout 120 python3 scripts/bench_quick.py --conns 500 --requests 200 --size 256 --mode http --label "threads=1,500conns,HTTP" || echo "  （压测超时或失败）"
kill "$P1" 2>/dev/null

echo
echo "=============== 完成 ==============="
