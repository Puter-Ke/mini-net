#!/usr/bin/env bash
# 完整测试：构建 + 单测 + 接口自动化(pytest) + 混沌 + C++ 压测对照
#   bash scripts/remote_full_test.sh
set -uo pipefail
cd "${GITHUB_WORKSPACE:-$PWD}"
PORT=8080
ulimit -n 65535 2>/dev/null || true

echo "================ 1/6 构建 ================"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null || { echo "配置失败"; exit 1; }
if ! cmake --build build -j"$(nproc)" > /tmp/build.log 2>&1; then
  echo "编译失败："; grep -E "error:" /tmp/build.log | head -20; exit 1
fi
echo "构建成功；产物："
find build -maxdepth 2 -type f -executable \( -name mini_net_server -o -name echo_bench \) 2>/dev/null | while read -r f; do
  echo "  $f ($(stat -c %s "$f") 字节)"
done
BENCH_BIN=$(find build -maxdepth 2 -type f -executable -name echo_bench | head -1)
echo "压测客户端路径：${BENCH_BIN:-未找到}"

echo
echo "================ 2/6 单元/集成测试 ================"
timeout 300 ctest --test-dir build --timeout 30 --output-on-failure > /tmp/ctest.log 2>&1
CTEST_RC=$?
echo "ctest 返回码: $CTEST_RC"
grep -E "tests passed|Total Test time" /tmp/ctest.log | tail -2
if [ "$CTEST_RC" != "0" ]; then
  echo "--- 失败的测试 ---"
  grep -A8 "The following tests FAILED" /tmp/ctest.log
  echo "--- 断言详情 ---"
  grep -E "Failure|Expected|Which is|Value of|actual" /tmp/ctest.log | head -20
fi

echo
echo "================ 3/6 接口自动化测试（pytest）================"
./build/mini_net_server "$PORT" 30 4 > /tmp/server_api.log 2>&1 &
API_PID=$!
sleep 1.5
python3 -m pytest tests/api/test_shorturl.py --base-url "http://127.0.0.1:$PORT" -q > /tmp/pytest.log 2>&1
echo "pytest 退出码: $?"
tail -3 /tmp/pytest.log
echo "--- 失败用例名 ---"
grep -E "^FAILED|^ERROR" /tmp/pytest.log | head -20
echo "--- 失败断言摘要 ---"
grep -E "AssertionError" /tmp/pytest.log | head -10
kill "$API_PID" 2>/dev/null; sleep 1

echo
echo "================ 4/6 混沌/异常测试 ================"
./build/mini_net_server "$PORT" 30 4 > /tmp/server_chaos.log 2>&1 &
CHAOS_PID=$!
sleep 1.5
timeout 180 python3 tests/chaos/chaos_test.py --base-url "http://127.0.0.1:$PORT" 2>&1 | tail -20 || echo "  （混沌测试返回非零或超时）"
if kill -0 "$CHAOS_PID" 2>/dev/null; then
  echo "  [通过] 服务进程在混沌测试后仍然存活"
else
  echo "  [失败] 服务进程被混沌测试搞崩了"
fi
kill "$CHAOS_PID" 2>/dev/null; sleep 1

echo
echo "================ 5/6 C++ 压测客户端：1 线程 vs 4 线程 ================"
for t in 1 4; do
  ./build/mini_net_server "$PORT" 30 "$t" > /tmp/server_$t.log 2>&1 &
  pid=$!; sleep 1.5
  echo "--- IO 线程 = $t ---"
  timeout 180 "$BENCH_BIN" --host 127.0.0.1 --port "$PORT" --conns 200 --requests 200 --size 256 --mode http 2>&1 | tail -14 || echo "  （该场景失败/超时）"
  kill "$pid" 2>/dev/null; sleep 1
done

echo
echo "================ 6/6 业务正确性回归（短链全链路）================"
./build/mini_net_server "$PORT" 30 4 > /tmp/server_end.log 2>&1 &
END_PID=$!
sleep 1.5
BODY='{"url":"https://example.com/full-test"}'
RESP=$(printf 'POST /api/shorten HTTP/1.1\r\nHost: t\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s' "${#BODY}" "$BODY" | timeout 5 nc -q 1 127.0.0.1 "$PORT" | tr -d '\r')
echo "$RESP" | head -1
CODE=$(echo "$RESP" | grep -o '"code":"[^"]*"' | head -1 | cut -d'"' -f4)
printf 'GET /%s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n' "$CODE" | timeout 5 nc -q 1 127.0.0.1 "$PORT" | tr -d '\r' | head -4
kill "$END_PID" 2>/dev/null

echo
echo "================ 全部完成 ================"
