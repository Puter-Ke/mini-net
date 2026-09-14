#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mini-net 混沌 / 异常测试（Chaos Test）

一句话说明这份文件在干什么：
    别人测试是「按说明书正常用」，混沌测试是「故意使坏」——
    把网线拔了、把请求发一半、发一堆乱码、开一堆慢连接赖着不走……
    然后看服务端会不会崩、会不会把正常用户饿死、崩完还能不能自己恢复。

真实世界的对应（这些都是线上真会发生的）：
    - 用户手机进了电梯 / Wi-Fi 断了      → 连接是 RST（啪一下断开），不是正常的挥手再见
    - 端口扫描器、爬虫、写错协议号的客户端 → 畸形 HTTP 请求
    - 慢速攻击（Slowloris）              → 用极慢的速度发请求头，把连接资源占满
    - 客户端进程被 kill -9               → 内核直接发 RST

判定「服务还活着」不能只看进程在不在，还要看它还能不能干活，所以每个场景跑完都会做
一次「自愈检查」（见 check_service_healthy）：健康检查 + 短链闭环 + 指标不倒退。

────────────────────────────── 怎么用 ──────────────────────────────
1) 独立运行（会打印一张中文汇总表，有失败退出码为 1）：

       python3 tests/chaos/chaos_test.py --host 127.0.0.1 --port 8080

   可选参数：--conns 并发连接数（默认 200）、--base-url、--pid 服务进程号。

2) 被 pytest 收集（文件名 chaos_test.py 匹配 pytest 默认的 *_test.py 规则）：

       pytest tests/chaos/chaos_test.py -v

   此时每个场景是一个 def test_xxx()，配置从环境变量读；服务没起来就自动 skip。

环境变量（都有默认值，不设也能跑）：
    MININET_HOST      服务地址，默认 127.0.0.1
    MININET_PORT      服务端口，默认 8080
    MININET_BASE_URL  HTTP 基地址，默认 http://<host>:<port>
    CHAOS_CONNS       并发连接数，默认 200（慢速攻击最多用 50 条）
    MININET_PID       服务进程号；给了就顺便看一眼 /proc/<pid> 还在不在
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import socket
import struct
import sys
import threading
import time
import unicodedata

# ── 允许用到的三种第三方/内置依赖，全部做「缺了也能活」处理 ──────────────────
# requests：发正常 HTTP 请求（健康检查、闭环验证）用。没装的话给出中文提示。
try:
    import requests
except ImportError:  # pragma: no cover - 本机只做语法检查，运行机装好即可
    requests = None

# pytest：被 pytest 收集时才需要。没装也能用 `python3 本文件` 独立跑。
try:
    import pytest
except ImportError:  # pragma: no cover
    pytest = None

# allure：可选的测试报告，没装就全部跳过，绝不影响测试本身。
try:
    import allure
except ImportError:  # pragma: no cover
    allure = None


# ══════════════════════════════ 基础配置 ══════════════════════════════

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8080
DEFAULT_CONNS = 200          # 场景 4 用多少条连接
SLOW_ATTACK_MAX_CONNS = 50   # 慢速攻击最多开多少条，开太多会把自己机器也拖垮
HTTP_TIMEOUT = 5             # 所有正常 HTTP 请求的超时（秒）

SLOW_ATTACK_DURATION = 10.0  # 慢速攻击持续多久（硬上限，绝不无限跑）
SLOW_ATTACK_INTERVAL = 0.5   # 慢连接每 0.5 秒发 1 个字节

# 场景 4 里「正常连接」的成功率下限。留 5% 是给极端调度抖动，
# 不是用来掩盖 bug 的：真掉了请求一般会远低于这个值。
NORMAL_SUCCESS_RATE_MIN = 0.95

# /metrics 必须包含的字段（接口契约，见 docs/API.md）
REQUIRED_METRICS = (
    "uptime_seconds",
    "alive_connections",
    "total_connections",
    "total_requests",
    "total_bytes_in",
    "shorten_total",
    "redirect_total",
)

# 各场景对应的「真实故障」说明，打印给用户看的大白话
WHAT_PARTIAL_RST = (
    "用户网络突然断了（手机进电梯 / Wi-Fi 掉线 / 客户端被 kill -9）："
    "请求只发了一半，客户端内核直接甩一个 RST 过来，连正常的挥手再见都没有。"
)
WHAT_MALFORMED = (
    "端口扫描器、写错协议号的客户端、被中间设备改坏的报文："
    "发出去的根本不像一个合法的 HTTP 请求，服务端解析时会遇到各种脏数据。"
)
WHAT_SLOWLORIS = (
    "慢速攻击（Slowloris）：攻击者开一堆连接，每隔半秒才挤 1 个字节的请求头，"
    "永远不说「我说完了」，把服务端的连接资源一条条占住，想让正常用户连不进来。"
)
WHAT_KILL_HALF = (
    "高并发下客户端批量掉线：一半用户在服务端刚要回包时强行断开（RST），"
    "服务端写响应会直接写到一个已经死掉的连接上。"
)
WHAT_FRAGMENT = (
    "网络传输天然会把数据切碎或攒在一起：一条请求被拆成几段才到，"
    "或者好几条请求被粘成一大坨一次到达（这就是面试常问的拆包 / 粘包）。"
)


class Config:
    """把「命令行参数 / 环境变量 / 默认值」合成一份配置。

    优先级：命令行 > 环境变量 > 默认值。
    """

    def __init__(self, host=None, port=None, conns=None, base_url=None, pid=None):
        self.host = host or os.environ.get("MININET_HOST") or DEFAULT_HOST
        self.port = int(
            port if port is not None else (os.environ.get("MININET_PORT") or DEFAULT_PORT)
        )
        self.conns = int(
            conns if conns is not None else (os.environ.get("CHAOS_CONNS") or DEFAULT_CONNS)
        )
        env_base = os.environ.get("MININET_BASE_URL")
        self.base_url = (base_url or env_base or f"http://{self.host}:{self.port}").rstrip("/")
        self.pid = str(pid) if pid is not None else os.environ.get("MININET_PID")

    def host_port(self):
        """拼出 Host 头里要写的那一串，例如 127.0.0.1:8080。"""
        return f"{self.host}:{self.port}"


# 全局唯一的一份配置。pytest 收集时用环境变量建；独立运行时 main() 会用命令行参数重建。
CONFIG = Config()


def _host_port():
    return CONFIG.host_port()


def _force_utf8_stdout():
    """Windows 控制台默认是 GBK 编码，直接print中文可能报错，这里尽量切成 UTF-8。"""
    for stream in (sys.stdout, sys.stderr):
        with contextlib.suppress(Exception):
            stream.reconfigure(encoding="utf-8", errors="replace")


_force_utf8_stdout()


# ══════════════════════════ 小工具：中文表格对齐 ══════════════════════════

def _disp_width(text):
    """算一个字符串在终端里占几格宽（中文/全角字符算 2 格），这样表格才对得齐。"""
    width = 0
    for ch in text:
        width += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return width


def _pad(text, width):
    """按显示宽度右侧补空格。"""
    return text + " " * max(0, width - _disp_width(text))


# ══════════════════════════ socket 手写小工具 ══════════════════════════

# read_until_idle 的「结局」取值
REASON_PEER_CLOSED = "peer_closed"     # 对端关了连接（读到 0 字节）
REASON_GOT_RESPONSE = "got_response"   # 已经拿到一条完整的 HTTP 响应
REASON_IDLE_TIMEOUT = "idle_timeout"   # 对端既没回也没关，我们等超时了
REASON_RESET = "reset"                 # 被对端 RST 掉了
REASON_IO_ERROR = "io_error"           # 其它网络错误


def open_conn(host, port, timeout=HTTP_TIMEOUT):
    """建立一个 TCP 连接，并设置超时——绝不能让自己的脚本挂死。"""
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def close_quietly(sock):
    """正常关闭连接（发 FIN 挥手），期间任何报错都无视。

    真实世界对应：用户正常关掉浏览器标签页。
    """
    if sock is None:
        return
    with contextlib.suppress(OSError):
        sock.shutdown(socket.SHUT_RDWR)
    with contextlib.suppress(OSError):
        sock.close()


def rst_close(sock):
    """用「直接 RST」的方式断开连接，而不是正常的 FIN 挥手。

    原理：SO_LINGER 的 l_onoff=1 / l_linger=0 会让内核在 close() 的瞬间把
    缓冲区里没发完的数据丢掉，并直接甩一个 RST 给对方。
    真实世界对应：手机进电梯、Wi-Fi 断线、对端进程被 kill -9。

    小知识：struct linger 在 Linux 上是两个 int（8 字节），在 Windows 上是两个
    u_short（4 字节）。所以两个都试一遍，哪个内核肯接受就用哪个。
    """
    for fmt in ("ii", "hh"):
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack(fmt, 1, 0))
            break
        except (OSError, struct.error):
            continue
    with contextlib.suppress(OSError):
        sock.close()


def _try_parse_one(buf):
    """从缓冲区开头切出一条完整的 HTTP 响应（按 Content-Length 判断长度）。

    切不出来（还没收全）返回 None；切得出来返回 (这条响应, 剩下的字节)。
    """
    idx = buf.find(b"\r\n\r\n")
    if idx < 0:
        return None
    head = buf[:idx]
    content_length = 0
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            with contextlib.suppress(ValueError):
                content_length = int(line.split(b":", 1)[1].strip())
    end = idx + 4 + content_length
    if len(buf) < end:
        return None
    return buf[:end], buf[end:]


def _status_of(raw):
    """从原始响应里抠出状态码，抠不出来返回 None。"""
    try:
        return int(raw.split(b" ")[1])
    except (IndexError, ValueError):
        return None


def read_until_idle(sock, timeout, max_bytes=65536):
    """读服务端回包，一直读到「有完整响应 / 对端关闭 / 超时 / 出错」为止。

    返回 (收到的字节, 结局)，结局的取值见上面的 REASON_* 常量。
    注意：一旦已经拿到一条完整的 HTTP 响应就立刻返回，不会傻等到超时。
    """
    sock.settimeout(timeout)
    buf = b""
    try:
        while len(buf) < max_bytes:
            chunk = sock.recv(4096)
            if not chunk:
                return buf, REASON_PEER_CLOSED
            buf += chunk
            if _try_parse_one(buf) is not None:
                return buf, REASON_GOT_RESPONSE
    except socket.timeout:
        return buf, REASON_IDLE_TIMEOUT
    except ConnectionResetError:
        return buf, REASON_RESET
    except OSError:
        return buf, REASON_IO_ERROR
    return buf, REASON_GOT_RESPONSE


def recv_http_responses(sock, expected, timeout):
    """从连接上连着解析出 expected 条完整响应（粘包场景用）。

    返回 (响应列表, 没解析完的残留字节)。读不满 expected 条就把已经读到的返回，
    由调用方拿数量去判定通过与否。
    """
    buf = b""
    messages = []
    deadline = time.monotonic() + timeout
    while len(messages) < expected:
        parsed = _try_parse_one(buf)
        if parsed is not None:
            msg, buf = parsed
            messages.append(msg)
            continue
        remain = deadline - time.monotonic()
        if remain <= 0:
            break
        sock.settimeout(remain)
        try:
            chunk = sock.recv(65536)
        except (socket.timeout, OSError):
            break
        if not chunk:
            break
        buf += chunk
    return messages, buf


def build_request(method, path, body=None, keep_alive=True):
    """拼一条标准的 HTTP/1.1 请求。body 是 bytes（不传就是没有 body 的 GET）。"""
    parts = [f"{method} {path} HTTP/1.1", f"Host: {_host_port()}"]
    if body is not None:
        parts.append("Content-Type: application/json")
        parts.append(f"Content-Length: {len(body)}")
    parts.append("Connection: keep-alive" if keep_alive else "Connection: close")
    head = "\r\n".join(parts) + "\r\n\r\n"
    return head.encode("utf-8") + (body or b"")


def _json_body(url):
    return json.dumps({"url": url}).encode("utf-8")


# ══════════════════════════ HTTP 封装（requests） ══════════════════════════

def _request(method, path, **kwargs):
    """统一走这一个口子发 HTTP 请求：自动拼 base_url + 默认 5 秒超时。

    所有地方都用它，避免有的地方忘了设超时把整个测试挂死。
    """
    if requests is None:
        raise RuntimeError("缺少 requests 库，先执行：pip install requests")
    kwargs.setdefault("timeout", HTTP_TIMEOUT)
    return requests.request(method, CONFIG.base_url + path, **kwargs)


def parse_metrics(text):
    """把 /metrics 的文本解析成 {指标名: 数值} 字典。"""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or " " not in line:
            continue
        name, _, value = line.partition(" ")
        try:
            out[name] = float(value)
        except ValueError:
            continue
    return out


def _business_round(timeout=HTTP_TIMEOUT):
    """走一遍正常业务闭环「建短链 → 跳转」，成功返回 True。

    慢速攻击进行中我们就靠它来验证一件事：正常用户有没有被慢连接饿死。
    """
    try:
        url = f"https://example.com/chaos/normal/{time.time_ns()}"
        resp = _request("POST", "/api/shorten", json={"url": url}, timeout=timeout)
        if resp.status_code != 201:
            return False
        code = (resp.json() or {}).get("code")
        if not code:
            return False
        resp = _request("GET", f"/{code}", allow_redirects=False, timeout=timeout)
        return resp.status_code == 302 and resp.headers.get("Location") == url
    except Exception:
        return False


# ══════════════════════════ 自愈检查（混沌测试的灵魂） ══════════════════════════

# 上一次采样到的 total_requests，用来判断计数器有没有「倒退」（倒退=服务偷偷重启过）
_HEALTH_STATE = {"last_total_requests": None}


def check_service_healthy(scenario_name=""):
    """每个场景跑完都要问一句：服务还活着吗？还能正常干活吗？

    做四件事（任何一件不通过，这个场景就算失败）：
      1) GET /healthz 是不是还在回 200 + body 恰好是 ok；
      2) 完整走一遍「建短链 → 跳转 → 查统计」，跳转地址必须和当初存进去的一模一样；
      3) GET /metrics 拿得到，而且 total_requests 和上一次采样比不能变少
         （变少说明计数器被清零了，通常意味着服务偷偷重启过——这就是「自愈」的反面）；
      4) 如果给了 MININET_PID，看一眼 /proc/<pid> 还在不在（进程没死）。

    返回 (是否健康, 中文说明)。
    """
    problems = []
    ok_bits = []

    # 1) 健康检查：这是服务自己说「我很好」的地方
    try:
        resp = _request("GET", "/healthz")
        if resp.status_code != 200:
            problems.append(f"GET /healthz 返回 {resp.status_code}（期望 200）")
        elif resp.text.strip() != "ok":
            problems.append(f"GET /healthz 的 body 是 {resp.text!r}（期望 ok）")
        else:
            ok_bits.append("健康检查 ok")
    except Exception as exc:
        problems.append(f"GET /healthz 请求失败：{type(exc).__name__}: {exc}")

    # 2) 业务闭环：活着不等于能干活的，得真建一条短链再跳一次
    url = f"https://example.com/chaos/health/{time.time_ns()}"
    code = None
    try:
        resp = _request("POST", "/api/shorten", json={"url": url})
        if resp.status_code != 201:
            problems.append(f"POST /api/shorten 返回 {resp.status_code}（期望 201）")
        else:
            payload = resp.json() or {}
            code = payload.get("code")
            if not code:
                problems.append(f"创建短链的返回里没有 code：{payload}")
            if payload.get("url") != url:
                problems.append(f"返回的 url 和提交的不一致：{payload.get('url')!r} != {url!r}")
    except Exception as exc:
        problems.append(f"POST /api/shorten 失败：{type(exc).__name__}: {exc}")

    if code:
        try:
            resp = _request("GET", f"/{code}", allow_redirects=False)
            if resp.status_code != 302:
                problems.append(f"GET /{code} 返回 {resp.status_code}（期望 302）")
            elif resp.headers.get("Location") != url:
                problems.append(
                    f"跳转地址对不上：拿到 {resp.headers.get('Location')!r}，期望 {url!r}"
                )
        except Exception as exc:
            problems.append(f"GET /{code} 失败：{type(exc).__name__}: {exc}")
        try:
            resp = _request("GET", f"/api/stats/{code}")
            if resp.status_code != 200:
                problems.append(f"GET /api/stats/{code} 返回 {resp.status_code}（期望 200）")
        except Exception as exc:
            problems.append(f"GET /api/stats/{code} 失败：{type(exc).__name__}: {exc}")
        if not problems:
            ok_bits.append("短链闭环正常")

    # 3) 指标：数字不能倒退，倒退 = 服务其实重启过
    try:
        resp = _request("GET", "/metrics")
        if resp.status_code != 200:
            problems.append(f"GET /metrics 返回 {resp.status_code}（期望 200）")
        else:
            metrics = parse_metrics(resp.text)
            missing = [name for name in REQUIRED_METRICS if name not in metrics]
            if missing:
                problems.append(f"/metrics 少了字段：{'、'.join(missing)}")
            total = metrics.get("total_requests")
            last = _HEALTH_STATE["last_total_requests"]
            if total is None:
                problems.append("/metrics 里没有 total_requests，没法比较计数器")
            else:
                if last is not None and total < last:
                    problems.append(
                        f"total_requests 从 {last:.0f} 掉到 {total:.0f}，服务很可能重启过"
                    )
                _HEALTH_STATE["last_total_requests"] = total
                ok_bits.append(f"指标正常（total_requests={total:.0f}）")
    except Exception as exc:
        problems.append(f"GET /metrics 失败：{type(exc).__name__}: {exc}")

    # 4) 进程还在不在（这是唯一允许的「看进程」手段，不用 psutil）
    if CONFIG.pid:
        if os.path.isdir("/proc"):
            if not os.path.exists(f"/proc/{CONFIG.pid}"):
                problems.append(f"/proc/{CONFIG.pid} 不见了，服务进程已经死了")
            else:
                ok_bits.append(f"进程 /proc/{CONFIG.pid} 还在")
        else:
            ok_bits.append("当前系统没有 /proc，跳过进程存活检查")

    if problems:
        who = f"服务在「{scenario_name}」之后" if scenario_name else "服务"
        detail = f"{who}失去了自愈能力：" + "；".join(problems)
        return False, detail
    return True, "、".join(ok_bits)


# ══════════════════════════ 场景结果记录 ══════════════════════════

class ScenarioRecord:
    """一个场景的结果小账本：试了几次、成了几次、服务端主动关了几次、判定通过没有。"""

    def __init__(self, name, what):
        self.name = name
        self.what = what
        self.attempts = 0        # 尝试次数
        self.success = 0         # 成功次数
        self.server_closed = 0   # 服务端在我们还没拿到完整响应时先关了连接（读到空/被 RST）
        self.client_errors = 0   # 客户端侧的网络错误（连不上等）
        self.counters = {}       # 各场景自己的细分统计，键是中文标签
        self.notes = []          # 要打给用户看的补充说明
        self.passed = False
        self.detail = "还没跑"

    def bump(self, key, delta=1):
        self.counters[key] = self.counters.get(key, 0) + delta

    def rate(self):
        return (self.success / self.attempts * 100.0) if self.attempts else 0.0


def _print_scenario_intro(rec):
    """每个场景开始前，讲清楚「现在模拟的真实故障是什么」。"""
    print()
    print("=" * 78)
    print(f"场景 {rec.name}")
    print(f"  模拟的真实故障：{rec.what}")
    print("=" * 78)


def _pytest_skip(message):
    """pytest 收集时服务不可达就跳过；独立运行不该走到这里（会直接抛错）。"""
    if pytest is not None:
        pytest.skip(message)
    raise RuntimeError(message)


def _ensure_service_or_skip():
    """开跑前先确认服务在不在。"""
    if _service_reachable():
        return
    _pytest_skip(f"服务没起来（{CONFIG.host}:{CONFIG.port} 连不上），跳过混沌测试")


def _service_reachable(timeout=3):
    """只用一次 TCP 连接探活，开销最小。"""
    try:
        with contextlib.closing(
            socket.create_connection((CONFIG.host, CONFIG.port), timeout=timeout)
        ):
            return True
    except OSError:
        return False


def _finish_scenario(rec, scenario_ok, reason):
    """场景结尾统一做自愈检查，并把「场景判定」和「自愈检查」合起来定最终结论。"""
    healthy, health_detail = check_service_healthy(rec.name)
    if scenario_ok and healthy:
        rec.passed = True
        rec.detail = f"场景通过；自愈检查也通过（{health_detail}）"
    elif not scenario_ok and not healthy:
        rec.passed = False
        rec.detail = f"{reason}；另外{health_detail}"
    elif not scenario_ok:
        rec.passed = False
        rec.detail = reason
    else:
        rec.passed = False
        rec.detail = health_detail
    print(f"  自愈检查：{'通过' if healthy else '失败'}（{health_detail}）")
    print(f"  场景判定：{'通过' if rec.passed else '失败'} —— {rec.detail}")
    return rec


def _report(rec):
    """把场景结果打印出来（pytest 模式下也会被 pytest 捕获，失败时能看到）。"""
    print(
        f"  统计：尝试 {rec.attempts} 次，成功 {rec.success} 次"
        f"（{rec.rate():.1f}%），服务端主动关闭 {rec.server_closed} 次，"
        f"客户端侧错误 {rec.client_errors} 次"
    )
    for key, value in rec.counters.items():
        print(f"    - {key}：{value}")
    for note in rec.notes:
        print(f"    · {note}")
    _allure_attach(f"{rec.name} 结果", rec.detail)


def _allure_attach(name, text):
    """有 allure 就附一份结果进去，没有就算了。"""
    if allure is None:
        return
    with contextlib.suppress(Exception):
        allure.attach(  # pragma: no cover - 只在装了 allure 的环境执行
            text, name=name, attachment_type=allure.attachment_type.TEXT
        )


def _allure_step(title):
    """pytest 里可以包一层 allure 步骤；没装 allure 就什么都不做。"""
    return allure.step(title) if allure is not None else contextlib.nullcontext()


def _allure_feature(feature, story=None, title=None):
    """给用例贴 allure 标签；没装 allure 时装饰器原样返回函数。"""

    def decorator(func):
        if allure is None:
            return func
        func = allure.feature(feature)(func)  # pragma: no cover
        if story:
            func = allure.story(story)(func)  # pragma: no cover
        if title:
            func = allure.title(title)(func)  # pragma: no cover
        return func

    return decorator


def _parametrize(argvalues, ids):
    """装了 pytest 就按参数拆分用例（报告里每个变体一条）；没装就原样返回函数。"""
    if pytest is None:
        return lambda func: func
    return pytest.mark.parametrize("variant_name", argvalues, ids=ids)


# ══════════════════════════════ 场景 1 ══════════════════════════════
# 请求发到一半就 RST 断开。
# 真实世界：用户手机进电梯、Wi-Fi 掉线、客户端进程被 kill -9。

def _partial_request_prefix():
    """一条只写了一半的请求：连请求头都没写完，在 Content-Len 处戛然而止。"""
    return (
        b"POST /api/shorten HTTP/1.1\r\n"
        b"Host: " + _host_port().encode("ascii", "replace") + b"\r\n"
        b"Content-Type: application/json\r\n"
        b"Content-Len"
    )


def _one_partial_rst():
    """连上 → 发半截请求 → 立刻 RST。返回 True 表示这次「使坏」成功送出去了。"""
    sock = None
    try:
        sock = open_conn(CONFIG.host, CONFIG.port, timeout=3)
        sock.sendall(_partial_request_prefix())
        rst_close(sock)   # 关键：不发 FIN，直接 RST
        sock = None
        return True
    except OSError:
        return False
    finally:
        close_quietly(sock)


def _run_partial_request_rst():
    rec = ScenarioRecord("1. 请求中途断开（RST）", WHAT_PARTIAL_RST)
    _print_scenario_intro(rec)

    batch_size = max(1, min(CONFIG.conns, 100))  # 一轮的量，别让脚本自己太慢
    rounds = 2                                   # 重复一整个批次
    for round_no in range(1, rounds + 1):
        for _ in range(batch_size):
            rec.attempts += 1
            if _one_partial_rst():
                rec.success += 1
            else:
                rec.client_errors += 1
        print(f"  第 {round_no}/{rounds} 批：{batch_size} 条连接「发一半就 RST」已送出")

    rec.notes.append(
        "服务端这一侧看不到任何报错才算正常：读到一个半截请求后，"
        "它应该默默把连接回收掉（日志里可能有 EPIPE/ECONNRESET，那是预期内的）。"
    )
    scenario_ok = rec.success == rec.attempts
    reason = f"有 {rec.client_errors} 条连接连发送都没成功，客户端侧就有问题"
    _finish_scenario(rec, scenario_ok, reason)
    _report(rec)
    return rec


# ══════════════════════════════ 场景 2 ══════════════════════════════
# 各种畸形 HTTP 请求：乱码、超长头、裸 \n、半个请求、空请求、非法版本、超长 URL、NULL 字节。
# 判定标准：服务端不能崩，响应要么是合法 HTTP 响应，要么干净地关连接（读到 0 字节）。

class MalformedVariant:
    """一个畸形请求变体。"""

    def __init__(self, name, desc, expect, data, read_timeout, pre_sleep, allow_hung):
        self.name = name                  # 变体名（会出现在 pytest 用例 id 里）
        self.desc = desc                  # 这是什么，大白话
        self.expect = expect              # 期望服务端怎么反应
        self.data = data                  # 要发出去的字节
        self.read_timeout = read_timeout  # 这次最多等多久
        self.pre_sleep = pre_sleep        # 发之前先干等几秒（空请求变体用）
        self.allow_hung = allow_hung      # 是否允许「既不回也不关」（服务端有自己的读超时）


def _spec_random_bytes(hp):
    """乱码请求行：64 个随机二进制字节 + CRLFCRLF。"""
    return (
        os.urandom(64) + b"\r\n\r\n",
        3.0, 0.0, False,
    )


def _spec_huge_header(hp):
    """单个请求头 > 8 KiB。契约说超过 8 KiB 应该回 431，直接关连接也算通过。"""
    data = b"GET /healthz HTTP/1.1\r\nHost: " + hp.encode() + b"\r\nX-Chaos-Big: " + b"A" * 9000 + b"\r\n\r\n"
    return (data, 3.0, 0.0, False)


def _spec_bare_lf(hp):
    """裸 \\n 当行结束符（HTTP 规定必须是 \\r\\n）。宽容处理或拒绝都算通过。"""
    return (b"GET /healthz\n\n", 6.5, 0.0, True)


def _spec_half_body(hp):
    """头写完了、Content-Length 也声明了，但 body 只发一半就停住。"""
    body = b'{"url": "https://example.com/chaos/half-body"}'
    head = (
        b"POST /api/shorten HTTP/1.1\r\nHost: " + hp.encode()
        + b"\r\nContent-Type: application/json\r\nContent-Length: "
        + str(len(body)).encode() + b"\r\n\r\n"
    )
    half = body[: len(body) // 2]
    # 契约里读超时是 5 秒（超时回 408），所以我们等 6.5 秒好看到它的反应。
    return (head + half, 6.5, 0.0, True)


def _spec_empty(hp):
    """连上以后什么都不发，干等 1 秒（模拟连上就不说话的死连接）。"""
    return (b"", 1.0, 1.0, True)


def _spec_bad_version(hp):
    """非法 HTTP 版本号 HTTP/9.9。"""
    return (b"GET / HTTP/9.9\r\nHost: " + hp.encode() + b"\r\n\r\n", 3.0, 0.0, False)


def _spec_long_url(hp):
    """路径长 10000 字符的超长 URL，超过契约里 8 KiB 的请求行上限。"""
    data = b"GET /" + b"a" * 10000 + b" HTTP/1.1\r\nHost: " + hp.encode() + b"\r\n\r\n"
    return (data, 4.0, 0.0, False)


def _spec_null_bytes(hp):
    """头里塞 NULL 字节和非法控制字符（很多 C 语言解析器就是这么被搞崩的）。"""
    data = (
        b"GET /healthz HTTP/1.1\r\nHost: " + hp.encode()
        + b"\r\nX-Chaos-Bad: a\x00b\x01\x02c\x7f\r\n\r\n"
    )
    return (data, 3.0, 0.0, False)


# (变体名, 说明, 期望, 造数据的函数)
MALFORMED_SPECS = (
    (
        "乱码请求行",
        "发 64 个随机二进制字节再加 CRLFCRLF，等于对着服务端说胡话。",
        "回一个 4xx 错误，或者干脆把连接关掉",
        _spec_random_bytes,
    ),
    (
        "超长请求头（>8KiB）",
        "单个请求头的值塞了 9000 个字符，远超契约里 8 KiB 的上限。",
        "431（请求头过大）或直接关连接，两种都算通过",
        _spec_huge_header,
    ),
    (
        "裸 \\n 换行",
        "用 \\n 当行结束符（HTTP 规定必须是 \\r\\n）。",
        "宽容地当成正常请求，或者拒绝掉，两种都算通过",
        _spec_bare_lf,
    ),
    (
        "半个请求（body 只发一半）",
        "头写全了、也声明了 Content-Length，但 body 发一半就不发了。",
        "读超时后回 408 并关连接（契约是 5 秒），不崩就行",
        _spec_half_body,
    ),
    (
        "空请求（连上不说话）",
        "连上以后一个字节都不发，干等 1 秒，占着连接。",
        "服务端等着（稍后由读超时/空闲超时收拾），不崩就行",
        _spec_empty,
    ),
    (
        "非法 HTTP 版本",
        "请求行写成 GET / HTTP/9.9，一个根本不存在的协议版本。",
        "回 4xx/5xx（比如 505）或关连接",
        _spec_bad_version,
    ),
    (
        "超长 URL（10000 字符）",
        "路径塞了 10000 个字符，请求行远超 8 KiB 上限。",
        "431 或 414 之类的 4xx，或直接关连接",
        _spec_long_url,
    ),
    (
        "头里塞 NULL 和非法字符",
        "请求头里混进 \\x00、\\x01、\\x7f，C 语言字符串处理最容易在这里翻车。",
        "回 4xx 或关连接，绝对不能让进程崩掉",
        _spec_null_bytes,
    ),
)

_MALFORMED_NAMES = tuple(spec[0] for spec in MALFORMED_SPECS)


def build_malformed_variants(host_port=None):
    """按当前配置把 8 个畸形变体都造出来。"""
    hp = host_port or _host_port()
    variants = []
    for name, desc, expect, builder in MALFORMED_SPECS:
        data, read_timeout, pre_sleep, allow_hung = builder(hp)
        variants.append(MalformedVariant(name, desc, expect, data, read_timeout, pre_sleep, allow_hung))
    return variants


def _send_malformed(variant):
    """发一个畸形请求，返回 (是否通过, 中文结果描述, 分类标签)。"""
    sock = None
    try:
        sock = open_conn(CONFIG.host, CONFIG.port, timeout=3)
        if variant.pre_sleep:
            time.sleep(variant.pre_sleep)
        if variant.data:
            sock.sendall(variant.data)
        data, reason = read_until_idle(sock, variant.read_timeout)
    except OSError as exc:
        return False, f"客户端侧网络错误：{type(exc).__name__}: {exc}", "客户端错误"
    finally:
        close_quietly(sock)
    return _judge_malformed(variant, data, reason)


def _judge_malformed(variant, data, reason):
    """判定服务端的反应算不算「通过」。"""
    if data[:5] == b"HTTP/":
        status = _status_of(data)
        if status is None:
            return False, f"回了一段像 HTTP 但状态行读不出来的东西：{data[:60]!r}", "响应不合法"
        kind = "HTTP 4xx/5xx" if status >= 400 else "HTTP 2xx/3xx"
        return True, f"返回 {status} —— 合法 HTTP 响应", kind

    if reason == REASON_PEER_CLOSED:
        if not data:
            return True, "一个字节都没回，干净地关掉了连接", "无响应直接关闭"
        return False, f"回了一段不是 HTTP 的字节（{len(data)} 字节）：{data[:40]!r}", "响应不合法"

    if reason == REASON_RESET:
        return True, "一个字节都没回，直接 RST 关掉了连接", "无响应直接关闭"

    if reason == REASON_IDLE_TIMEOUT:
        text = f"服务端没回也没关，挂了 {variant.read_timeout} 秒"
        if variant.allow_hung:
            return True, text + "（这个变体允许服务端先挂着，等它自己的读/空闲超时）", "挂着未处理"
        return False, text + "，像被这条脏请求卡住了", "挂着未处理"

    if reason == REASON_GOT_RESPONSE:
        return False, f"收到的字节不像 HTTP 响应：{data[:40]!r}", "响应不合法"

    return False, f"网络错误：{reason}", "客户端错误"


def _run_malformed_variant(variant):
    """跑单个畸形变体（发脏请求 + 分类 + 立刻做一次自愈检查）。"""
    print(f"  ▸ {variant.name}：{variant.desc}")
    print(f"    期望：{variant.expect}")
    with _allure_step(f"发送畸形请求：{variant.name}"):
        ok, desc, kind = _send_malformed(variant)
    print(f"    服务端反应：{desc} → {'通过' if ok else '失败'}")
    healthy, health_detail = check_service_healthy(f"畸形 HTTP：{variant.name}")
    if not healthy:
        print(f"    自愈检查：失败（{health_detail}）")
        return False, desc, kind, health_detail
    return ok, desc, kind, ""


def _run_malformed_http():
    rec = ScenarioRecord("2. 畸形 HTTP 请求", WHAT_MALFORMED)
    _print_scenario_intro(rec)
    variants = build_malformed_variants()

    for variant in variants:
        rec.attempts += 1
        ok, desc, kind, health_detail = _run_malformed_variant(variant)
        rec.bump(kind)
        if kind == "无响应直接关闭":
            rec.server_closed += 1
        if ok and not health_detail:
            rec.success += 1
        else:
            if health_detail:
                rec.notes.append(f"「{variant.name}」之后自愈检查失败：{health_detail}")
            else:
                rec.notes.append(f"「{variant.name}」判定失败：{desc}")

    rec.notes.append(
        "统计口径：『HTTP 4xx/5xx』是服务端给了一个正经错误响应（推荐做法），"
        "『无响应直接关闭』是它选择闷声关连接（契约里允许）。两种都不算错。"
    )
    scenario_ok = rec.success == rec.attempts
    reason = f"有 {rec.attempts - rec.success} 个畸形变体让服务端反应不正常"
    _finish_scenario(rec, scenario_ok, reason)
    _report(rec)
    return rec


# ══════════════════════════════ 场景 3 ══════════════════════════════
# 慢速攻击（Slowloris）：开 N 条连接，每隔 0.5 秒发 1 个字节，永远不说「我说完了」。
# 关键断言：这期间正常客户端还得能用（慢连接不能把正常连接饿死）。

def _slowloris_payload():
    """一条「永远写不完」的请求头：开头正常，后面挂着一大串填充字符。

    我们每隔 0.5 秒只往外推 1 个字节，所以结尾那个空行永远发不出去，
    服务端就一直等在那儿——这正是慢速攻击要占住的资源。
    """
    head = (
        "GET /healthz HTTP/1.1\r\n"
        f"Host: {_host_port()}\r\n"
        "User-Agent: slowloris-chaos\r\n"
        "X-Filler: "
    ).encode("utf-8")
    return head + b"z" * 4096


def _slow_client(payload, state, lock, stop_event, duration):
    """一条慢连接的工作线程：慢慢挤字节，同时偷看服务端有没有把我们踢掉。"""
    sock = None
    closed_by_server = False
    try:
        sock = socket.create_connection((CONFIG.host, CONFIG.port), timeout=5)
        sock.settimeout(0.0)  # 非阻塞：发和收都立刻返回，方便边发边看
        deadline = time.monotonic() + duration
        pos = 0
        while time.monotonic() < deadline and not stop_event.is_set():
            if pos < len(payload):
                try:
                    sock.send(payload[pos : pos + 1])  # 一次只发 1 个字节
                    pos += 1
                except BlockingIOError:
                    pass
                except OSError:
                    break
            # 偷看服务端有没有回包 / 关连接（非阻塞，读不到就抛 BlockingIOError）
            try:
                chunk = sock.recv(64)
                if chunk == b"":
                    _bump(state, lock, "服务端主动关闭")
                    closed_by_server = True
                    break
                _bump(state, lock, "服务端回了内容")
                closed_by_server = True
                break
            except BlockingIOError:
                pass
            except ConnectionResetError:
                _bump(state, lock, "服务端 RST")
                closed_by_server = True
                break
            except OSError:
                closed_by_server = True
                break
            time.sleep(SLOW_ATTACK_INTERVAL)  # 每隔 0.5 秒才发下一个字节
        if not closed_by_server:
            _bump(state, lock, "一直挂着没被踢")
    except OSError:
        _bump(state, lock, "连接失败")
    finally:
        close_quietly(sock)


def _bump(state, lock, key, delta=1):
    with lock:
        state[key] = state.get(key, 0) + delta


def _run_slowloris():
    rec = ScenarioRecord("3. 慢速发送（Slowloris）", WHAT_SLOWLORIS)
    _print_scenario_intro(rec)

    conns = max(1, min(CONFIG.conns, SLOW_ATTACK_MAX_CONNS))
    payload = _slowloris_payload()
    state = {}
    lock = threading.Lock()
    stop_event = threading.Event()

    threads = [
        threading.Thread(
            target=_slow_client,
            args=(payload, state, lock, stop_event, SLOW_ATTACK_DURATION),
            daemon=True,
        )
        for _ in range(conns)
    ]
    print(f"  开 {conns} 条慢连接，每条每 {SLOW_ATTACK_INTERVAL} 秒发 1 个字节，"
          f"总共持续约 {SLOW_ATTACK_DURATION:.0f} 秒……")
    for thread in threads:
        thread.start()

    # ★ 关键断言：慢连接赖着不走的时候，正常用户还能不能用？
    normal_ok = 0
    normal_fail = 0
    deadline = time.monotonic() + SLOW_ATTACK_DURATION
    while time.monotonic() < deadline:
        if _business_round():
            normal_ok += 1
        else:
            normal_fail += 1
        time.sleep(0.5)
    print(f"  攻击期间穿插了 {normal_ok + normal_fail} 次正常用户请求："
          f"成功 {normal_ok}，失败 {normal_fail}")

    stop_event.set()
    join_deadline = time.monotonic() + 5.0
    for thread in threads:
        thread.join(max(0.1, join_deadline - time.monotonic()))

    # 这个场景真正的「尝试次数」是攻击期间穿插的正常请求次数——
    # 我们要统计的就是「慢连接还在的时候，正常用户到底能不能用」。
    rec.attempts = normal_ok + normal_fail
    rec.success = normal_ok
    rec.server_closed = state.get("服务端主动关闭", 0)
    rec.bump("慢连接条数", conns)
    for key in ("服务端主动关闭", "服务端 RST", "服务端回了内容", "一直挂着没被踢", "连接失败"):
        if key in state:
            rec.bump(key, state[key])
    rec.notes.append(
        f"慢连接结局：服务端主动关了 {rec.server_closed} 条（说明它有在读超时/空闲超时兜底），"
        f"{state.get('一直挂着没被踢', 0)} 条到我们收手时还挂着。两种都能接受，"
        "只要能撑住不崩、不影响正常用户就行。"
    )
    if normal_fail:
        rec.notes.append(
            f"注意：攻击期间正常请求失败了 {normal_fail} 次 —— 慢连接把正常用户饿死了，"
            "这是这份混沌测试最想抓到的那个 bug。"
        )
    scenario_ok = (normal_fail == 0 and normal_ok > 0)
    if normal_ok == 0:
        reason = "攻击期间正常用户的请求一次都没成功，服务端已经不能服务正常流量了"
    elif normal_fail:
        reason = f"攻击期间正常请求失败了 {normal_fail} 次，慢连接把正常用户饿死了"
    else:
        reason = ""
    _finish_scenario(rec, scenario_ok, reason)
    _report(rec)
    return rec


# ══════════════════════════════ 场景 4 ══════════════════════════════
# 并发开 N 条连接，一半「发完请求立刻 RST」，另一半正常收发，看正常那半的成功率。

def _kill_half_chaos_worker(barrier, tally, lock, url):
    """坏用户：发完完整请求立刻 RST（服务端刚要回包就发现对面死了）。"""
    sock = None
    try:
        sock = open_conn(CONFIG.host, CONFIG.port, timeout=5)
        request = build_request("POST", "/api/shorten", body=_json_body(url), keep_alive=False)
        barrier.wait(timeout=10)   # 大家一起动手，制造真正的并发
        sock.sendall(request)
        rst_close(sock)
        sock = None
        _bump(tally, lock, "坏用户已发完并RST")
    except (OSError, threading.BrokenBarrierError):
        _bump(tally, lock, "坏用户出错")
    finally:
        close_quietly(sock)


def _kill_half_normal_worker(barrier, tally, lock, url, expected_status):
    """好用户：发完整请求，老老实实等响应，必须拿到 201。"""
    sock = None
    try:
        sock = open_conn(CONFIG.host, CONFIG.port, timeout=5)
        request = build_request("POST", "/api/shorten", body=_json_body(url), keep_alive=False)
        barrier.wait(timeout=10)
        sock.sendall(request)
        data, reason = read_until_idle(sock, HTTP_TIMEOUT)
        status = _status_of(data) if data[:5] == b"HTTP/" else None
        if status == expected_status:
            _bump(tally, lock, "正常用户成功")
        else:
            _bump(tally, lock, "正常用户失败")
            if not data:
                _bump(tally, lock, "正常连接被服务端先关掉")
    except (OSError, threading.BrokenBarrierError):
        _bump(tally, lock, "正常用户失败")
        _bump(tally, lock, "正常用户出错")
    finally:
        close_quietly(sock)


def _run_kill_half():
    rec = ScenarioRecord("4. 并发 kill 一半连接", WHAT_KILL_HALF)
    _print_scenario_intro(rec)

    total = max(2, CONFIG.conns)
    chaos_n = total // 2
    normal_n = total - chaos_n
    barrier = threading.Barrier(total)
    tally = {}
    lock = threading.Lock()

    print(f"  一次开 {total} 条连接：{chaos_n} 条发完就 RST，{normal_n} 条正常收发")
    threads = []
    for i in range(chaos_n):
        url = f"https://example.com/chaos/rst/{time.time_ns()}-{i}"
        threads.append(
            threading.Thread(
                target=_kill_half_chaos_worker,
                args=(barrier, tally, lock, url),
                daemon=True,
            )
        )
    for i in range(normal_n):
        url = f"https://example.com/chaos/ok/{time.time_ns()}-{i}"
        threads.append(
            threading.Thread(
                target=_kill_half_normal_worker,
                args=(barrier, tally, lock, url, 201),
                daemon=True,
            )
        )
    for thread in threads:
        thread.start()
    join_deadline = time.monotonic() + 30.0
    for thread in threads:
        thread.join(max(0.1, join_deadline - time.monotonic()))

    ok = tally.get("正常用户成功", 0)
    fail = tally.get("正常用户失败", 0)
    rate = (ok / normal_n * 100.0) if normal_n else 0.0
    print(f"  正常连接：成功 {ok} / {normal_n}（{rate:.1f}%），失败 {fail}")
    print(f"  坏连接：{tally.get('坏用户已发完并RST', 0)} / {chaos_n} 条完成了「发完就 RST」")

    rec.attempts = total
    rec.success = ok + tally.get("坏用户已发完并RST", 0)
    rec.server_closed = tally.get("正常连接被服务端先关掉", 0)
    for key, value in tally.items():
        rec.bump(key, value)
    rec.notes.append(
        f"判定线：正常连接成功率 ≥ {NORMAL_SUCCESS_RATE_MIN * 100:.0f}%。"
        "坏用户那半边拿不到响应是正常的（它自己先啪一下断了），"
        "重点是正常用户不能被连累。"
    )
    scenario_ok = (normal_n > 0 and ok / normal_n >= NORMAL_SUCCESS_RATE_MIN)
    reason = (
        f"正常连接成功率只有 {rate:.1f}%，低于 {NORMAL_SUCCESS_RATE_MIN * 100:.0f}% 的及格线"
        "（一半连接被 RST 不该牵连另一半）"
    )
    _finish_scenario(rec, scenario_ok, reason)
    _report(rec)
    return rec


# ══════════════════════════════ 场景 5 ══════════════════════════════
# 半包（拆包）和粘包。

def _fragmented_round(round_no):
    """半包：把一条完整请求拆成 3 段，段之间停 0.05 秒再发（模拟网络把包切碎了）。"""
    url = f"https://example.com/chaos/frag/{time.time_ns()}-{round_no}"
    request = build_request("POST", "/api/shorten", body=_json_body(url), keep_alive=False)
    third = len(request) // 3
    chunks = [request[:third], request[third : third * 2], request[third * 2 :]]
    sock = None
    try:
        sock = open_conn(CONFIG.host, CONFIG.port, timeout=HTTP_TIMEOUT)
        for chunk in chunks:
            sock.sendall(chunk)
            time.sleep(0.05)   # 故意慢一点，逼服务端经历「收不全」的状态
        data, _reason = read_until_idle(sock, HTTP_TIMEOUT)
        status = _status_of(data) if data[:5] == b"HTTP/" else None
        if status != 201:
            return False, f"拆成 3 段发出去，服务端回了 {status}（期望 201）"
        try:
            payload = json.loads(data.split(b"\r\n\r\n", 1)[1])
        except (ValueError, IndexError):
            return False, "拆包后拿到的响应 body 不是合法 JSON"
        if (payload or {}).get("url") != url:
            return False, "响应里的 url 和提交的不一致"
        return True, ""
    except OSError as exc:
        return False, f"拆包时网络出错：{type(exc).__name__}: {exc}"
    finally:
        close_quietly(sock)


def _coalesced_round(round_no, pipelined=3):
    """粘包：把 3 条完整请求拼成一大坨，一次 sendall 全发出去。"""
    urls = [f"https://example.com/chaos/glue/{time.time_ns()}-{round_no}-{i}" for i in range(pipelined)]
    # 注意：粘包场景故意用 keep-alive，让 3 条请求走同一条连接
    blob = b"".join(build_request("POST", "/api/shorten", body=_json_body(u), keep_alive=True) for u in urls)
    sock = None
    try:
        sock = open_conn(CONFIG.host, CONFIG.port, timeout=HTTP_TIMEOUT)
        sock.sendall(blob)   # 一次全发出去，服务端可能一次全读到
        messages, leftover = recv_http_responses(sock, pipelined, HTTP_TIMEOUT)
        if len(messages) != pipelined:
            return False, f"一次发了 {pipelined} 条请求，只解析回 {len(messages)} 条响应"
        for msg, url in zip(messages, urls):
            status = _status_of(msg)
            if status != 201:
                return False, f"粘包里的某条请求回了 {status}（期望 201）"
            try:
                payload = json.loads(msg.split(b"\r\n\r\n", 1)[1])
            except (ValueError, IndexError):
                return False, "粘包里的响应 body 不是合法 JSON"
            if payload.get("url") != url:
                return False, f"粘包里的响应串位了：{payload.get('url')!r} != {url!r}"
        if leftover:
            return False, f"解析完 {pipelined} 条响应后还多出 {len(leftover)} 字节垃圾数据"
        return True, ""
    except OSError as exc:
        return False, f"粘包时网络出错：{type(exc).__name__}: {exc}"
    finally:
        close_quietly(sock)


def _run_fragmentation():
    rec = ScenarioRecord("5. 半包 + 粘包", WHAT_FRAGMENT)
    _print_scenario_intro(rec)

    frag_rounds = 5
    glue_rounds = 3
    for i in range(frag_rounds):
        rec.attempts += 1
        ok, why = _fragmented_round(i)
        if ok:
            rec.success += 1
        else:
            rec.notes.append(f"第 {i + 1} 轮拆包失败：{why}")
        print(f"  拆包（一条请求掰成 3 段发）第 {i + 1}/{frag_rounds} 轮：{'通过' if ok else '失败 ' + why}")

    for i in range(glue_rounds):
        rec.attempts += 1
        ok, why = _coalesced_round(i)
        if ok:
            rec.success += 1
        else:
            rec.notes.append(f"第 {i + 1} 轮粘包失败：{why}")
        print(f"  粘包（3 条请求拼成一大坨一次发）第 {i + 1}/{glue_rounds} 轮：{'通过' if ok else '失败 ' + why}")

    rec.bump("拆包轮数", frag_rounds)
    rec.bump("粘包轮数", glue_rounds)
    rec.bump("粘包请求条数", glue_rounds * 3)
    scenario_ok = rec.success == rec.attempts
    reason = f"有 {rec.attempts - rec.success} 轮拆包/粘包没能拿到正确响应"
    _finish_scenario(rec, scenario_ok, reason)
    _report(rec)
    return rec


# ══════════════════════════════ pytest 用例 ══════════════════════════════
# 每个场景一个 def test_xxx()，不能带必填参数，配置全部从环境变量读。
# 服务不可达时 skip（不是 fail），方便在没有服务的机器上批量跑。

@_allure_feature("混沌测试", story="连接层", title="请求发到一半强行 RST")
def test_partial_request_then_rst():
    """场景 1：请求发一半就 RST 断开（模拟用户网络突然断了）。"""
    _ensure_service_or_skip()
    rec = _run_partial_request_rst()
    assert rec.passed, rec.detail


@_parametrize(list(_MALFORMED_NAMES), list(_MALFORMED_NAMES))
@_allure_feature("混沌测试", story="协议解析", title="畸形 HTTP 请求")
def test_malformed_http(variant_name):
    """场景 2：一个一个试畸形请求，服务端不能崩。"""
    _ensure_service_or_skip()
    variant = next(v for v in build_malformed_variants() if v.name == variant_name)
    ok, desc, kind, health_detail = _run_malformed_variant(variant)
    rec = ScenarioRecord(f"2. 畸形 HTTP：{variant.name}", WHAT_MALFORMED)
    rec.attempts = 1
    rec.success = 1 if (ok and not health_detail) else 0
    rec.bump(kind)
    if kind == "无响应直接关闭":
        rec.server_closed = 1
    rec.passed = ok and not health_detail
    rec.detail = health_detail or desc
    _report(rec)
    assert rec.passed, rec.detail


@_allure_feature("混沌测试", story="资源耗尽", title="慢速发送 Slowloris")
def test_slowloris():
    """场景 3：慢速攻击期间，正常用户还得能用。"""
    _ensure_service_or_skip()
    rec = _run_slowloris()
    assert rec.passed, rec.detail


@_allure_feature("混沌测试", story="并发断开", title="并发 kill 一半连接")
def test_kill_half_connections():
    """场景 4：一半连接发完就 RST，另一半必须照常拿到响应。"""
    _ensure_service_or_skip()
    rec = _run_kill_half()
    assert rec.passed, rec.detail


@_allure_feature("混沌测试", story="TCP 边界", title="半包与粘包")
def test_fragmented_and_coalesced():
    """场景 5：拆包（一条请求分 3 段发）和粘包（3 条请求拼一起发）都要正确。"""
    _ensure_service_or_skip()
    rec = _run_fragmentation()
    assert rec.passed, rec.detail


# ══════════════════════════════ 独立运行的编排与汇总 ══════════════════════════════

SCENARIOS = (
    _run_partial_request_rst,
    _run_malformed_http,
    _run_slowloris,
    _run_kill_half,
    _run_fragmentation,
)


def print_summary(records):
    """打印中文汇总表（自己算宽度，不用第三方表格库）。"""
    header = ("场景", "尝试", "成功", "成功率", "服务端主动关闭", "判定")
    rows = []
    for rec in records:
        rows.append(
            (
                rec.name,
                str(rec.attempts),
                str(rec.success),
                f"{rec.rate():.1f}%",
                str(rec.server_closed),
                "通过" if rec.passed else "失败",
            )
        )
    widths = [max(_disp_width(row[i]) for row in (header,) + tuple(rows)) for i in range(len(header))]

    print()
    print("=" * 78)
    print("混沌测试汇总")
    print("=" * 78)
    title_line = "  ".join(_pad(header[i], widths[i]) for i in range(len(header)))
    print(title_line)
    print("-" * _disp_width(title_line))
    for row in rows:
        print("  ".join(_pad(row[i], widths[i]) for i in range(len(row))))
    print("-" * _disp_width(title_line))
    print(
        "口径说明：『服务端主动关闭』＝我们还没读到完整响应，服务端就把连接关掉了"
        "（读到 0 字节或被 RST）。对畸形请求场景来说这是正常反应；对正常请求场景来说就是问题。"
    )

    failed = [rec for rec in records if not rec.passed]
    print()
    if not failed:
        print("结论：服务在全部混沌场景下存活且自愈正常")
    else:
        print(f"结论：有 {len(failed)} 个场景没通过：")
        for rec in failed:
            # 场景名前面已经打过了，这里把详情里重复的那一遍“服务在「场景名」之后”去掉
            detail = rec.detail.replace(f"服务在「{rec.name}」之后失去了自愈能力", "服务失去了自愈能力")
            print(f"  - {rec.name}：{detail}")
    return not failed


def main(argv=None):
    """独立运行的入口：跑完所有场景，打印中文报告，返回退出码（0 成功 / 1 有失败 / 2 服务不可达）。"""
    parser = argparse.ArgumentParser(
        description="mini-net 混沌测试：故意制造真实网络故障，验证服务不崩、能自愈。"
    )
    parser.add_argument("--host", default=None, help=f"服务地址（默认 {DEFAULT_HOST}，也可用 MININET_HOST）")
    parser.add_argument("--port", type=int, default=None, help=f"服务端口（默认 {DEFAULT_PORT}，也可用 MININET_PORT）")
    parser.add_argument(
        "--conns",
        type=int,
        default=None,
        help=f"并发连接数（默认 {DEFAULT_CONNS}，也可用 CHAOS_CONNS；慢速攻击最多用 {SLOW_ATTACK_MAX_CONNS} 条）",
    )
    parser.add_argument("--base-url", default=None, help="HTTP 基地址（默认 http://<host>:<port>）")
    parser.add_argument("--pid", default=None, help="服务进程号；给了就顺便盯一眼 /proc/<pid> 还在不在")
    args = parser.parse_args(argv)

    global CONFIG
    CONFIG = Config(
        host=args.host,
        port=args.port,
        conns=args.conns,
        base_url=args.base_url,
        pid=args.pid,
    )

    print("=" * 78)
    print("mini-net 混沌测试（故意使坏，看服务崩不崩、能不能自愈）")
    print("=" * 78)
    print(f"  目标服务   ：{CONFIG.host}:{CONFIG.port}")
    print(f"  HTTP 基地址：{CONFIG.base_url}")
    print(f"  并发连接数 ：{CONFIG.conns}（慢速攻击用 {max(1, min(CONFIG.conns, SLOW_ATTACK_MAX_CONNS))} 条）")
    print(f"  进程探活   ：{'/proc/' + CONFIG.pid if CONFIG.pid else '未指定 MININET_PID，跳过'}")
    print("  大概耗时   ：30~40 秒")

    if not _service_reachable():
        print()
        print(f"[错误] 服务连不上（{CONFIG.host}:{CONFIG.port}），混沌测试没法开始。")
        print("       先启动服务，例如：./build/mini_net_server 8080")
        return 2

    # 先做一次基线自检，顺便告诉用户「接口到底实现到哪一步了」
    print()
    print("基线自检（跑之前先确认服务是健康的）……")
    healthy, health_detail = check_service_healthy("基线自检")
    if healthy:
        print(f"  基线自检通过：{health_detail}")
    else:
        print(f"  基线自检没过：{health_detail}")
        print("  提示：如果服务还停在「回声服务器」阶段（HTTP 接口没实现，M5 才做），")
        print("        下面所有场景都会失败，这是预期的，等接口做完再跑这份混沌测试。")

    records = []
    for scenario in SCENARIOS:
        try:
            records.append(scenario())
        except KeyboardInterrupt:
            print("\n[中断] 收到 Ctrl+C，提前结束。")
            break
        except Exception as exc:  # 场景自己炸了也不能让整份报告丢了
            name = getattr(scenario, "__name__", str(scenario))
            print(f"\n[错误] 场景 {name} 自身抛异常：{type(exc).__name__}: {exc}")
            rec = ScenarioRecord(name, "场景自身异常")
            rec.passed = False
            rec.detail = f"场景自身抛异常：{type(exc).__name__}: {exc}"
            records.append(rec)

    all_passed = print_summary(records)
    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())
