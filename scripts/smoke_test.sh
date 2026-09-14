#!/usr/bin/env bash
# 第 1 周验收脚本：一键检查 mini-net 是否达标
# 用法：bash scripts/smoke_test.sh [端口]
#      内存小的机器可以调小并发：CONNS=500 bash scripts/smoke_test.sh 8080
set -uo pipefail

PORT="${1:-8080}"
BIN="./build/mini_net_server"
CONNS="${CONNS:-1000}"
PASS=0; FAIL=0
ok()   { echo "  [通过] $*"; PASS=$((PASS+1)); }
bad()  { echo "  [失败] $*"; FAIL=$((FAIL+1)); }
line() { echo; echo "=== $* ==="; }

line "0. 前置检查"
if [ ! -x "$BIN" ]; then
  echo "  找不到 $BIN，先执行：cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
  exit 1
fi
command -v nc >/dev/null || { echo "  缺 nc，执行：sudo apt install -y netcat-openbsd"; exit 1; }
ok "二进制存在，nc 可用"

line "1. 启动服务"
"$BIN" "$PORT" > /tmp/mini_net.log 2>&1 &
PID=$!
sleep 1
if kill -0 "$PID" 2>/dev/null; then ok "进程存活 (PID=$PID)"; else
  bad "进程启动就退出了，日志："; cat /tmp/mini_net.log; exit 1
fi
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then ok "端口 $PORT 正在监听"; else bad "端口 $PORT 没有监听"; fi

FD_BASE=$(ls /proc/$PID/fd 2>/dev/null | wc -l)
echo "  fd 基线（空载）：$FD_BASE"
ok "基线已记录"

line "2. HTTP 接口测试（M5 之后服务端讲 HTTP/1.1）"
OUT=$(printf 'GET /healthz HTTP/1.1\r\nHost: smoke\r\nConnection: close\r\n\r\n' | timeout 5 nc -q 1 127.0.0.1 "$PORT" 2>/dev/null | tr -d '\r')
if echo "$OUT" | head -1 | grep -q "200 OK"; then ok "GET /healthz 返回 200 OK"; else
  bad "HTTP 响应异常，收到的是：[$(echo "$OUT" | head -1)]"
fi
if echo "$OUT" | tail -1 | grep -q "^ok$"; then ok "响应体为 ok"; else bad "响应体不是 ok"; fi

# 短链全链路：创建 → 跳转
BODY='{"url":"https://example.com/smoke-test-target"}'
RESP=$(printf 'POST /api/shorten HTTP/1.1\r\nHost: smoke\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s' "${#BODY}" "$BODY" | timeout 5 nc -q 1 127.0.0.1 "$PORT" 2>/dev/null | tr -d '\r')
CODE=$(echo "$RESP" | grep -o '"code":"[^"]*"' | head -1 | cut -d'"' -f4)
if [ -n "$CODE" ]; then ok "创建短链成功，短码=$CODE"; else bad "创建短链失败：$RESP"; fi

if [ -n "$CODE" ]; then
  R2=$(printf 'GET /%s HTTP/1.1\r\nHost: smoke\r\nConnection: close\r\n\r\n' "$CODE" | timeout 5 nc -q 1 127.0.0.1 "$PORT" 2>/dev/null | tr -d '\r')
  if echo "$R2" | grep -q "302 Found" && echo "$R2" | grep -q "Location: https://example.com/smoke-test-target"; then
    ok "短链跳转 302 且 Location 正确"
  else
    bad "短链跳转异常：$(echo "$R2" | head -1)"
  fi
fi

line "3. 并发连接测试（$CONNS 个）"
echo "  正在建立连接，可能需要十几秒……"
for i in $(seq 1 "$CONNS"); do
  timeout 8 bash -c "exec 3<>/dev/tcp/127.0.0.1/$PORT; sleep 5" >/dev/null 2>&1 &
done
sleep 3
if kill -0 "$PID" 2>/dev/null; then ok "服务端在 $CONNS 并发下没有崩溃"; else
  bad "服务端崩了（这是最有价值的一条报错，去看 /tmp/mini_net.log）"
fi
FD_PEAK=$(ls /proc/$PID/fd 2>/dev/null | wc -l)
echo "  fd 峰值：$FD_PEAK（正常应接近 $((FD_BASE + CONNS))）"

# 等客户端自己超时断开（客户端 sleep 5 后关闭；不要用 wait，它会连服务端一起等，卡死）
sleep 8
if kill -0 "$PID" 2>/dev/null; then ok "客户端断开后服务端仍在"; else bad "客户端断开导致服务端退出（连接清理有 bug）"; fi
FD_AFTER=$(ls /proc/$PID/fd 2>/dev/null | wc -l)
echo "  fd 回收后：$FD_AFTER（应该回到 $FD_BASE 附近）"
if [ "$FD_AFTER" -le $((FD_BASE + 10)) ]; then ok "fd 已回收，没有连接泄漏"; else
  bad "fd 泄漏：$FD_AFTER - $FD_BASE = $((FD_AFTER - FD_BASE)) 个没关掉"
fi

line "4. 收尾"
kill "$PID" 2>/dev/null; sleep 1
kill -0 "$PID" 2>/dev/null && { bad "kill 之后进程还在（退出流程没写）"; kill -9 "$PID"; } || ok "服务端能正常退出"

line "结果"
echo "  通过 $PASS 项，失败 $FAIL 项"
[ "$FAIL" -eq 0 ] && echo "  第 1 周验收通过：把这些数字抄进 docs/RESULTS.md" \
                   || echo "  先修失败项。每条失败都是面试时能讲的真实故事，记进 docs/PITFALLS.md"
exit 0
