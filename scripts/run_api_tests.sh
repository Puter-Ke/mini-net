#!/usr/bin/env bash
# 一键跑接口自动化测试：构建 -> 启动服务 -> 跑 pytest -> 打印汇总 -> 关服务
#
# 用法：
#   bash scripts/run_api_tests.sh                    # 默认 8080 端口
#   bash scripts/run_api_tests.sh --port 9090        # 换个端口
#   bash scripts/run_api_tests.sh --no-build         # 已经编好了，跳过构建（省时间）
#   bash scripts/run_api_tests.sh --run-slow         # 连"慢用例"（要等 5 秒以上）一起跑
#   bash scripts/run_api_tests.sh -- -k TestRedirect # "--" 后面的参数原样传给 pytest
#   bash scripts/run_api_tests.sh --base-url http://192.168.1.5:8080   # 测远程服务（不启动本地服务）
#
# 退出码：0 = 全部通过；1 = 有用例失败；2 = 环境有问题（缺依赖 / 构建失败 / 服务起不来）
set -uo pipefail

PORT=8080
BASE_URL=""
DO_BUILD=1
PYTEST_EXTRA=()

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR" || exit 2

BIN="./build/mini_net_server"
TEST_DIR="${API_TEST_DIR:-tests/api}"
LOG_SERVER="/tmp/mini_net_api_server.log"
LOG_PYTEST="/tmp/mini_net_api_pytest.log"
SERVER_PID=""

PASS_CNT=0
FAIL_CNT=0

ok()   { echo "  [通过] $*"; }
bad()  { echo "  [失败] $*"; }
info() { echo "  [信息] $*"; }
line() { echo; echo "=== $* ==="; }

usage() {
  sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
  exit 0
}

# ---------------- 1. 解析命令行参数 ----------------
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help)      usage ;;
    --port)         PORT="${2:-}"; shift 2 ;;
    --port=*)       PORT="${1#*=}"; shift ;;
    --base-url)     BASE_URL="${2:-}"; shift 2 ;;
    --base-url=*)   BASE_URL="${1#*=}"; shift ;;
    --no-build)     DO_BUILD=0; shift ;;
    --run-slow)     PYTEST_EXTRA+=(--run-slow); shift ;;
    --)             shift; PYTEST_EXTRA+=("$@"); break ;;
    *)              PYTEST_EXTRA+=("$1"); shift ;;
  esac
done

if ! [[ "$PORT" =~ ^[0-9]+$ ]] || [ "$PORT" -lt 1 ] || [ "$PORT" -gt 65535 ]; then
  echo "[错误] 端口不合法：$PORT"
  exit 2
fi
REMOTE_MODE=0
if [ -z "$BASE_URL" ]; then
  BASE_URL="http://127.0.0.1:$PORT"
else
  REMOTE_MODE=1     # 指定了 --base-url，就认为是测别人已经跑着的服务，本脚本不负责起服务
fi

# 服务端进程一定要关掉，哪怕脚本中途被 Ctrl+C 打断
cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    echo
    echo "正在关闭服务端 (PID=$SERVER_PID)……"
    kill "$SERVER_PID" 2>/dev/null
    for _ in $(seq 1 30); do
      kill -0 "$SERVER_PID" 2>/dev/null || break
      sleep 0.1
    done
    if kill -0 "$SERVER_PID" 2>/dev/null; then
      echo "  服务端没能在 3 秒内退出，直接强杀（这说明退出流程有 bug，值得记进 docs/PITFALLS.md）"
      kill -9 "$SERVER_PID" 2>/dev/null
    else
      echo "  服务端已正常退出"
    fi
  fi
}
trap cleanup EXIT INT TERM

line "0. 前置检查"
command -v cmake >/dev/null 2>&1 || { bad "缺 cmake：sudo apt install -y cmake g++"; exit 2; }
ok "cmake 可用"

if ! command -v python3 >/dev/null 2>&1; then
  bad "缺 python3：sudo apt install -y python3 python3-pip"
  exit 2
fi
if ! python3 -c "import pytest" >/dev/null 2>&1; then
  bad "缺 pytest，先装依赖：pip3 install pytest requests"
  exit 2
fi
if ! python3 -c "import requests" >/dev/null 2>&1; then
  bad "缺 requests，先装依赖：pip3 install pytest requests"
  exit 2
fi
ok "python3 + pytest + requests 可用（$(python3 --version 2>&1)）"

if [ ! -d "$TEST_DIR" ]; then
  bad "找不到测试目录 $TEST_DIR"
  exit 2
fi
ok "测试目录：$TEST_DIR"

# ---------------- 2. 构建 ----------------
if [ "$REMOTE_MODE" -eq 0 ]; then
  line "1. 构建服务端"
  if [ "$DO_BUILD" -eq 1 ]; then
    if cmake -B build -DCMAKE_BUILD_TYPE=Release >/tmp/mini_net_cmake.log 2>&1 &&
       cmake --build build -j >>/tmp/mini_net_cmake.log 2>&1; then
      ok "构建成功（日志：/tmp/mini_net_cmake.log）"
    else
      bad "构建失败，最后 30 行日志："
      tail -n 30 /tmp/mini_net_cmake.log
      exit 2
    fi
  else
    info "按 --no-build 跳过构建"
  fi

  if [ ! -x "$BIN" ]; then
    bad "找不到可执行文件 $BIN，去掉 --no-build 重跑一次"
    exit 2
  fi
  ok "服务端程序：$BIN"

  # ---------------- 3. 启动服务 ----------------
  line "2. 启动服务（端口 $PORT）"
  "$BIN" "$PORT" > "$LOG_SERVER" 2>&1 &
  SERVER_PID=$!
  info "服务端 PID=$SERVER_PID，日志：$LOG_SERVER"

  # 等端口真正能连上再跑测试：进程起来了但还没 listen 的话，前几条用例会莫名其妙地失败
  READY=0
  for _ in $(seq 1 100); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then break; fi
    if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then READY=1; break; fi
    sleep 0.1
  done

  if [ "$READY" -ne 1 ]; then
    bad "服务端起不来或者端口 $PORT 没在监听，服务端日志："
    tail -n 30 "$LOG_SERVER"
    exit 2
  fi
  ok "端口 $PORT 已经可以连接"

  # 顺便探一下 HTTP 接口有没有实现：M5 还没做完的话，接口用例全红是正常的，
  # 这里提前打个招呼，免得你以为是测试脚本写错了。
  HEALTH="$(python3 - "$BASE_URL" <<'PY' 2>/dev/null || true
import sys, urllib.request
try:
    with urllib.request.urlopen(sys.argv[1] + "/healthz", timeout=3) as r:
        print(r.read().decode("utf-8", "replace").strip())
except Exception:
    print("")
PY
)"
  if [ "$HEALTH" = "ok" ]; then
    ok "GET /healthz 返回 ok，HTTP 接口已就绪"
  else
    info "GET /healthz 没有返回 ok（收到的是「${HEALTH:-空}」）"
    info "如果 M5（HTTP 解析 + 短链业务）还没实现，接口用例会大面积失败，这是预期的，不是脚本的问题"
  fi
else
  line "1-2. 跳过构建和启动（--base-url 指向远程服务）"
  info "被测服务：$BASE_URL"
fi

# ---------------- 4. 跑 pytest ----------------
line "3. 运行接口测试"
ALLURE_ARGS=()
if python3 -c "import allure" >/dev/null 2>&1; then
  mkdir -p build/allure-results
  ALLURE_ARGS+=("--alluredir=build/allure-results" "--clean-alluredir")
  info "检测到 allure-pytest，结果目录：build/allure-results"
else
  info "没装 allure（可选），跳过报告收集。想装：pip3 install allure-pytest"
fi

# 这里故意不开 set -e：pytest 返回非 0 是"用例失败"，属于正常路径，
# 脚本要继续往下走打印汇总，而不是直接中断。
python3 -m pytest "$TEST_DIR" -v --base-url "$BASE_URL" "${ALLURE_ARGS[@]+"${ALLURE_ARGS[@]}"}" "${PYTEST_EXTRA[@]+"${PYTEST_EXTRA[@]}"}" 2>&1 | tee "$LOG_PYTEST"
RC=${PIPESTATUS[0]}

# ---------------- 5. 汇总 ----------------
line "4. 汇总"
SUMMARY_LINE="$(grep -E '^=+ .*(passed|failed|error|no tests ran).* =+$' "$LOG_PYTEST" | tail -n 1 || true)"
if [ -n "$SUMMARY_LINE" ]; then
  info "pytest 结论：${SUMMARY_LINE//=/}"
fi

PASSED="$(grep -oE '[0-9]+ passed' "$LOG_PYTEST" | tail -n 1 | grep -oE '[0-9]+' || true)"
FAILED="$(grep -oE '[0-9]+ failed' "$LOG_PYTEST" | tail -n 1 | grep -oE '[0-9]+' || true)"
ERRORS="$(grep -oE '[0-9]+ error' "$LOG_PYTEST" | tail -n 1 | grep -oE '[0-9]+' || true)"
PASS_CNT="${PASSED:-0}"
FAIL_CNT="$(( ${FAILED:-0} + ${ERRORS:-0} ))"

echo "  通过 $PASS_CNT 条，失败 $FAIL_CNT 条"
echo "  完整用例日志：$LOG_PYTEST"
[ -n "$SERVER_PID" ] && echo "  服务端日志  ：$LOG_SERVER"

if [ "$FAIL_CNT" -gt 0 ] && [ -n "$SERVER_PID" ]; then
  echo
  info "服务端日志最后 15 行（失败时先看这里，往往一眼就能看出是解析崩了还是返回码不对）："
  tail -n 15 "$LOG_SERVER" | sed 's/^/    /'
fi

if command -v allure >/dev/null 2>&1 && [ -d build/allure-results ]; then
  if allure generate build/allure-results -o build/allure-report --clean >/dev/null 2>&1; then
    info "allure 报告已生成：build/allure-report/index.html"
  else
    info "allure 报告生成失败（不影响测试结论）"
  fi
fi

line "结果"
if [ "$RC" -eq 0 ]; then
  echo "  接口测试全部通过 ✅  （把这一轮的结论记进 docs/PROGRESS.md）"
elif [ "$RC" -eq 5 ]; then
  bad "一条用例都没跑到（检查 pytest 的收集路径和 --run-slow 之类的过滤条件）"
else
  bad "有 $FAIL_CNT 条用例没通过。每条失败都先分清是「服务端 bug」还是「测试写错了」，"
  echo "       服务端 bug 记进 docs/PITFALLS.md，这是面试时最好的素材。"
fi

exit "$RC"
