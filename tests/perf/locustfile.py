#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mini-net 性能压测脚本（Locust）。

一句话说明：这一个文件有两副面孔——
  1) 当 Locust 的"压测脚本"用（有界面 / 无界面都行）；
  2) 单独运行时，当"结果校验脚本"用（读 CSV，判断有没有达标，不达标就让 CI 变红）。

================================ 怎么跑 ================================
第 0 步：装依赖（只需一次）
    pip install locust

第 1 步：先把服务跑起来，确认它是活的
    curl http://127.0.0.1:8080/healthz      # 应该看到 ok

第 2 步：三种跑法任选

  (a) 有界面（自己点着玩、看曲线图最方便）
        locust -f tests/perf/locustfile.py
      然后浏览器打开 http://localhost:8089，填并发数和地址，点 Start。

  (b) 无界面（CI / 记录基线用，最常用）
        locust -f tests/perf/locustfile.py --headless -u 1000 -r 100 -t 60s \
               --csv=/tmp/mininet_perf --only-summary
      参数含义：-u 并发虚拟用户数(1000)  -r 每秒拉起多少个用户(100)  -t 跑多久(60 秒)

  (c) 用本文件自带的"压测 + 校验"一条龙（不用记 Locust 的那堆参数）
        python3 tests/perf/locustfile.py --users 1000 --spawn-rate 100 --run-time 60s

第 3 步（可选）：只校验已经跑出来的 CSV 结果。
    CI 里如果想"跑压测"和"判定结果"分成两个步骤，第二步这样跑：
        python3 tests/perf/locustfile.py --check-only --csv-prefix /tmp/mininet_perf
    任何 --help 都能看中文帮助：python3 tests/perf/locustfile.py --help

============================ 阈值怎么改（环境变量） ============================
所有阈值都能用环境变量临时覆盖，不用改代码。例如本机 4 核想把 P99 放宽到 10ms：
    PERF_P99_MS=10 python3 tests/perf/locustfile.py --users 200 --run-time 30s

可用的环境变量（括号里是默认值）：
    PERF_P99_MS             总体 P99 延迟上限，毫秒                (5)
    PERF_P50_MS             总体 P50 延迟上限，毫秒                (2)
    PERF_MAX_FAILURE_RATIO  失败率上限，0.001 就是 0.1%            (0.001)
    PERF_REDIRECT_QPS       跳转场景的最低吞吐（QPS）              (20000)
    PERF_SHORTEN_QPS        创建场景的最低吞吐（QPS）              (15000)
    PERF_MIN_REQUESTS       至少要有多少个请求才认这次结果         (1000)
    PERF_SLOW_REQUEST_MS    单个请求慢于多少毫秒就单独记为失败     (50)
    MININET_HOST            被测服务地址                           (http://127.0.0.1:8080)

======================== 1000 并发时机器要多少资源 ========================
压测机（跑 Locust 的这台）是要跟被测服务抢 CPU 的，所以有几句实话得先说清楚：
  - 4 核机器带 1000 并发非常勉强。Locust 本身是单进程 Python，发包能力通常只有
    1~3 万 QPS，很可能"客户端先撑不住"，于是测出来的数字偏保守，不能用来证伪目标。
  - 想真正压到 2 万 QPS 以上，建议：压测机 8 核以上；用 --processes 4 起多个
    Locust 进程分摊发包压力；最好再让压测机和被测服务分开在两台机器上。
  - 本机自测时把期望值调低一点（比如 PERF_REDIRECT_QPS=8000），此时看的是
    "链路通不通、有没有泄漏、延迟量级对不对"，而不是死磕绝对 QPS。
"""

from __future__ import annotations

import argparse
import csv
import itertools
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unicodedata
import uuid


# ===========================================================================
# 一、依赖与常量区
# ===========================================================================

# locust 只在"真的要压测"时才用得上。这里做容错导入：
# 万一某台机器没装 locust，本文件依然可以被 import，仍能当纯 CSV 校验脚本用，
# 而不是直接 ModuleNotFoundError 崩在启动那一步。
try:
    from locust import HttpUser, between, events, task

    LOCUST_AVAILABLE = True
    LOCUST_IMPORT_ERROR = ""
except Exception as _exc:  # pragma: no cover - 只在没装 locust 时走到
    LOCUST_AVAILABLE = False
    LOCUST_IMPORT_ERROR = str(_exc)


def _env_float(name: str, default: float) -> float:
    """读一个"小数"型环境变量；没设或者写成乱码就退回默认值（并给中文提醒）。"""
    raw = os.getenv(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        return float(raw)
    except ValueError:
        print(
            f"[配置] 环境变量 {name}={raw!r} 不是合法数字，改用默认值 {default}",
            file=sys.stderr,
        )
        return default


def _env_int(name: str, default: int) -> int:
    """读一个"整数"型环境变量；说明同 _env_float。"""
    raw = os.getenv(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        return int(raw)
    except ValueError:
        print(
            f"[配置] 环境变量 {name}={raw!r} 不是合法整数，改用默认值 {default}",
            file=sys.stderr,
        )
        return default


# 被测服务地址（去掉结尾的斜杠，免得拼出 //api/shorten 这种 URL）
HOST = os.getenv("MININET_HOST", "http://127.0.0.1:8080").rstrip("/")

# 接口地址（唯一事实来源：docs/API.md）
API_SHORTEN = "/api/shorten"
NAME_PREFIX_STATS = "/api/stats/"       # 后面拼短码
CODE_PATTERN = re.compile(r"^[0-9a-zA-Z]{1,8}$")

# 报告里的"聚合名"。
# 为什么必须有：跳转和查统计的 URL 里带着短码，1000 个用户就是 1000 个不同的 URL。
# 如果不强制聚合，Locust 会为每个 URL 单独出一行统计，报告会变成几千行没法看。
NAME_SHORTEN = "创建短链 POST /api/shorten"
NAME_REDIRECT = "跳转 GET /{code}"
NAME_STATS = "统计 GET /api/stats/{code}"
GROUP_NAMES = (NAME_SHORTEN, NAME_REDIRECT, NAME_STATS)
GROUP_LABELS = {
    NAME_SHORTEN: "创建短链",
    NAME_REDIRECT: "跳转",
    NAME_STATS: "统计",
}

# 流量配比：创建 30% / 跳转 60% / 统计 10%，也就是权重 3 : 6 : 1
TASK_WEIGHT_SHORTEN = 3
TASK_WEIGHT_REDIRECT = 6
TASK_WEIGHT_STATS = 1

# 单请求"太慢"的判定线（毫秒）。
# 为什么是 50ms：跳转只是内存里查一次表，正常都在 1ms 以内；能慢到 50ms，
# 说明是排队、锁竞争或者 GC 之类的系统性问题，值得单独报出来。
SLOW_REQUEST_MS = _env_float("PERF_SLOW_REQUEST_MS", 50.0)

# 运行级阈值。默认值来自项目文档的性能目标，CI 里可以直接用环境变量调。
THRESHOLDS = {
    "p99_ms": _env_float("PERF_P99_MS", 5.0),
    "p50_ms": _env_float("PERF_P50_MS", 2.0),
    "max_failure_ratio": _env_float("PERF_MAX_FAILURE_RATIO", 0.001),
    "redirect_qps": _env_float("PERF_REDIRECT_QPS", 20000.0),
    "shorten_qps": _env_float("PERF_SHORTEN_QPS", 15000.0),
    "min_requests": _env_int("PERF_MIN_REQUESTS", 1000),
}

# CSV 结果文件里我们必须用到的列（locust 固定会写这些表头）；缺了就报中文错，不让它崩。
CSV_COLUMNS_NEEDED = ("Name", "Request Count", "Failure Count", "Requests/s", "50%", "99%")

# locust 的"总计行"标记
AGGREGATED_NAME = "Aggregated"

# 每个虚拟用户一个编号，保证各用户的短链 URL 互不相同
USER_SEQ = itertools.count(1)


class CsvFormatError(Exception):
    """CSV 结果文件格式不对时抛出，消息一直是中文、能看懂的那种。"""


# ===========================================================================
# 二、工具函数（不依赖 locust，压测模式和纯校验模式都能用）
# ===========================================================================


def display_width(text: str) -> int:
    """算字符串在终端里占几格。

    中文字符在终端里占 2 格，直接用 len() 算宽度会让表格越对齐越歪，所以单独处理。
    """
    width = 0
    for ch in str(text):
        width += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return width


def pad(text: str, width: int) -> str:
    """按"显示宽度"左对齐补空格，专门给中文表格用。"""
    text = str(text)
    return text + " " * max(0, width - display_width(text))


def body_preview(resp, limit: int = 200) -> str:
    """取响应体的前 200 个字符，报错时能看清服务端到底回了什么。"""
    text = ""
    try:
        if isinstance(resp.text, str):
            text = resp.text
    except Exception:
        text = ""
    if not text:
        try:
            text = resp.content.decode("utf-8", "replace")
        except Exception:
            text = ""
    # 压成一行，免得把多行响应体糊满整个终端
    return text.replace("\r", " ").replace("\n", " ")[:limit]


def response_ms(resp) -> float:
    """取这次请求实际花了多少毫秒。

    优先用 requests 自带的 elapsed（真实网络往返耗时）。不同 locust 版本对上下文
    管理器复制的字段不完全一样，所以再兜一层：拿不到就退回 locust 内部的耗时记录；
    两条路都不通就返回 0（此时只是"慢请求"这条判据失效，不会误判成失败）。
    """
    elapsed = getattr(resp, "elapsed", None)
    if elapsed is not None:
        try:
            return float(elapsed.total_seconds()) * 1000.0
        except Exception:
            pass
    meta = getattr(resp, "_request_meta", None) or getattr(resp, "request_meta", None)
    if isinstance(meta, dict):
        try:
            return float(meta.get("response_time", 0.0))
        except Exception:
            pass
    return 0.0


def safe_json(resp):
    """尽力把响应体当 JSON 解析；解析不了就返回 None。

    这里绝不往外抛异常：一个用户解析失败不应该把整个虚拟用户打挂。
    """
    try:
        return resp.json()
    except Exception:
        return None


def judge_response(resp, ok_status, scene: str):
    """统一判定一个响应合不合格。

    返回 None 表示没问题；返回一段中文说明表示"这是失败，原因是……"。
    两条判据：
      1) 状态码必须在 ok_status 里（比如跳转只认 302）；
      2) 单请求耗时不能超过 SLOW_REQUEST_MS（默认 50ms）。
    抽成一个函数是为了三个任务用的是同一套标准，不会写着写着就各不相同。
    """
    if resp.status_code not in ok_status:
        expected = "/".join(str(s) for s in ok_status)
        return (
            f"{scene}：期望状态码 {expected}，实际 {resp.status_code}，"
            f"响应体前 200 字符：{body_preview(resp)}"
        )
    cost_ms = response_ms(resp)
    if cost_ms > SLOW_REQUEST_MS:
        return (
            f"慢请求：{scene} 单次耗时 {cost_ms:.1f} ms，超过单请求上限 "
            f"{SLOW_REQUEST_MS:.0f} ms（可用环境变量 PERF_SLOW_REQUEST_MS 调整），"
            f"响应体前 200 字符：{body_preview(resp)}"
        )
    return None


def percentile_from_response_times(response_times, pct: float) -> float:
    """兜底的分位数算法。

    locust 的 response_times 长这样：{响应时间(毫秒): 出现了几次}。
    把所有响应时间从小到大排好队，累计出现次数第一次超过 pct% 的那个值，就是分位数。
    （本函数存在的唯一理由见 entry_percentile 的说明。）
    """
    if not response_times:
        return 0.0
    try:
        items = sorted((float(rt), int(cnt)) for rt, cnt in response_times.items() if cnt)
    except Exception:
        return 0.0
    if not items:
        return 0.0
    total = sum(cnt for _, cnt in items)
    if total <= 0:
        return 0.0
    target = total * pct / 100.0
    seen = 0
    for response_time, count in items:
        seen += count
        if seen >= target:
            return response_time
    return items[-1][0]


def entry_percentile(entry, pct: float) -> float:
    """取某条统计的延迟分位数（毫秒）。

    优先用 locust 自带的 get_response_time_percentile()；
    但这个方法在不同 locust 版本里位置不一样（有的挂在 StatsEntry 上，有的只在内部的
    RequestStats 上），而且没数据时还可能直接抛异常。所以必须留一条"自己从
    response_times 算"的退路——否则换台机器、换个版本跑，脚本就崩了。
    """
    getter = getattr(entry, "get_response_time_percentile", None)
    if callable(getter):
        try:
            value = getter(pct / 100.0)
            if value is not None:
                return float(value)
        except Exception:
            pass  # 拿不到就落到下面的兜底算法
    return percentile_from_response_times(getattr(entry, "response_times", None), pct)


def calc_rps(entry, requests: int) -> float:
    """没有 total_rps 时的兜底：用"请求数 / (最后一次请求时刻 - 开始时刻)"估 QPS。"""
    start = getattr(entry, "start_time", None)
    last = getattr(entry, "last_request_timestamp", None)
    if start and last and last > start and requests:
        return float(requests) / (float(last) - float(start))
    return 0.0


def entry_to_stat(entry) -> dict:
    """把 locust 的一条统计（StatsEntry）转成普通字典，统一成我们自己的字段名。"""
    requests = int(getattr(entry, "num_requests", 0) or 0)
    failures = int(getattr(entry, "num_failures", 0) or 0)
    qps = getattr(entry, "total_rps", None)
    if qps is None:
        qps = calc_rps(entry, requests)
    return {
        "requests": requests,
        "failures": failures,
        "failure_ratio": (failures / requests) if requests else 0.0,
        "qps": float(qps or 0.0),
        "p50_ms": entry_percentile(entry, 50.0),
        "p99_ms": entry_percentile(entry, 99.0),
    }


def find_entry(stats, name: str):
    """按"聚合名"从 environment.stats 里取出一条统计。

    locust 2.x 的 entries 是以 (方法, 聚合名) 元组做 key；老版本是用 "GET /x" 字符串。
    两种都兼容一下，免得换个版本就取不到数据、报告全空。
    """
    entries = getattr(stats, "entries", None)
    if not entries:
        return None
    try:
        items = entries.items()
    except AttributeError:
        return None
    for key, entry in items:
        candidate = key[-1] if isinstance(key, tuple) else str(key)
        if candidate == name or candidate.endswith(name):
            return entry
    return None


def merge_stats(items) -> dict:
    """手工把多条统计合并成"总计"（没有 Aggregated 行时用）。

    QPS 直接相加；延迟取里面最差的那个——宁可保守，也不要报一个漂亮的假数字。
    """
    merged = {"requests": 0, "failures": 0, "qps": 0.0, "p50_ms": 0.0, "p99_ms": 0.0}
    for item in items:
        merged["requests"] += int(item.get("requests", 0) or 0)
        merged["failures"] += int(item.get("failures", 0) or 0)
        merged["qps"] += float(item.get("qps", 0.0) or 0.0)
        merged["p50_ms"] = max(merged["p50_ms"], float(item.get("p50_ms", 0.0) or 0.0))
        merged["p99_ms"] = max(merged["p99_ms"], float(item.get("p99_ms", 0.0) or 0.0))
    merged["failure_ratio"] = (
        merged["failures"] / merged["requests"] if merged["requests"] else 0.0
    )
    return merged


def summarize_stats(stats) -> dict:
    """把 locust 的 environment.stats 转成"规整字典"。

    规整成什么样（后面 check_thresholds 和 CSV 模式都吃这个格式）：
        {
          "total":  {"requests":.., "failures":.., "failure_ratio":.., "qps":.., "p50_ms":.., "p99_ms":..},
          "groups": {"创建短链 ...": {同上}, ...},
        }
    """
    groups = {}
    for name in GROUP_NAMES:
        entry = find_entry(stats, name)
        if entry is not None:
            groups[name] = entry_to_stat(entry)

    total_entry = getattr(stats, "total", None)
    if total_entry is not None:
        total = entry_to_stat(total_entry)
    else:
        total = merge_stats(list(groups.values()))
    return {"total": total, "groups": groups}


def check_thresholds(summary: dict) -> list:
    """拿规整后的统计字典对照阈值，返回中文违规说明列表；全都达标就返回空列表。

    为什么单独抽成一个"纯函数"（同样的输入永远给同样的输出、不碰网络也不碰文件）：
    压测结束时（locust 的钩子里）和"只校验 CSV"这两种模式，必须用同一套判定规则，
    抽出来才不会两边写着写着就跑偏了。
    """
    problems = []
    total = summary.get("total") or {}
    groups = summary.get("groups") or {}

    requests = float(total.get("requests", 0) or 0)
    failures = float(total.get("failures", 0) or 0)
    failure_ratio = float(total.get("failure_ratio", 0.0) or 0.0)

    # 1) 先看样本量：只发出去几百个请求的 P99 基本是噪声，结论没有意义
    if requests <= 0:
        problems.append(
            "一个请求都没发出去（服务没起来？地址写错了？端口不对？），无法判定性能"
        )
    elif requests < THRESHOLDS["min_requests"]:
        problems.append(
            f"请求总数只有 {int(requests)} 个，少于要求的 {THRESHOLDS['min_requests']} 个，"
            f"样本太少、结论不可信（请加大 --users 或 --run-time）"
        )

    # 2) 失败率：越低越好，默认要求 < 0.1%
    if requests > 0 and failure_ratio > THRESHOLDS["max_failure_ratio"]:
        problems.append(
            f"失败率 {failure_ratio * 100:.3f}% 超过上限 "
            f"{THRESHOLDS['max_failure_ratio'] * 100:.3f}%（失败 {int(failures)} / 总请求 {int(requests)}）"
        )

    # 3) 延迟：P99 是"最慢的那 1% 有多慢"，它超标说明偶发卡顿，比平均值更能说明问题
    if requests > 0:
        p99 = float(total.get("p99_ms", 0.0) or 0.0)
        p50 = float(total.get("p50_ms", 0.0) or 0.0)
        if p99 > THRESHOLDS["p99_ms"]:
            problems.append(
                f"总体 P99 延迟 {p99:.2f} ms 超过上限 {THRESHOLDS['p99_ms']:.2f} ms"
            )
        if p50 > THRESHOLDS["p50_ms"]:
            problems.append(
                f"总体 P50 延迟 {p50:.2f} ms 超过上限 {THRESHOLDS['p50_ms']:.2f} ms"
            )

    # 4) 分场景吞吐：跳转和创建各自的最低 QPS。
    #    注意 Locust 的 QPS 是"整轮平均"，包含了慢慢加并发的那段爬坡时间，所以偏保守。
    for name, key, label in (
        (NAME_REDIRECT, "redirect_qps", "跳转"),
        (NAME_SHORTEN, "shorten_qps", "创建短链"),
    ):
        item = groups.get(name)
        if item is None:
            problems.append(
                f"报告里找不到「{label}」场景（聚合名 {name}）的数据，这一轮可能根本没跑到"
            )
            continue
        qps = float(item.get("qps", 0.0) or 0.0)
        if qps < THRESHOLDS[key]:
            problems.append(
                f"{label}场景吞吐 {qps:,.0f} QPS 低于目标 {THRESHOLDS[key]:,.0f} QPS"
            )
    return problems


def _stat_row(label: str, item: dict) -> str:
    """渲染表格里的一行。"""
    requests = int(item.get("requests", 0) or 0)
    failures = int(item.get("failures", 0) or 0)
    ratio = float(item.get("failure_ratio", 0.0) or 0.0) * 100.0
    return (
        pad(label, 14)
        + pad(f"{requests:,}", 10)
        + pad(f"{failures:,}", 8)
        + pad(f"{ratio:.3f}%", 10)
        + pad(f"{float(item.get('qps', 0.0) or 0.0):,.0f}", 12)
        + pad(f"{float(item.get('p50_ms', 0.0) or 0.0):.2f}", 9)
        + pad(f"{float(item.get('p99_ms', 0.0) or 0.0):.2f}", 9)
    )


def format_report(summary: dict, problems: list) -> str:
    """把汇总字典渲染成一张中文表格（顺带把本轮阈值和违规清单也打出来）。"""
    total = summary.get("total") or {}
    groups = summary.get("groups") or {}

    lines = []
    lines.append("=" * 76)
    lines.append("mini-net 性能压测汇总")
    lines.append("=" * 76)
    lines.append(
        pad("场景", 14)
        + pad("请求数", 10)
        + pad("失败数", 8)
        + pad("失败率", 10)
        + pad("QPS", 12)
        + pad("P50(ms)", 9)
        + pad("P99(ms)", 9)
    )
    lines.append("-" * 76)
    for name in GROUP_NAMES:
        item = groups.get(name)
        if item is not None:
            lines.append(_stat_row(GROUP_LABELS.get(name, name), item))
    if groups:
        lines.append("-" * 76)
    lines.append(_stat_row("合计", total))

    lines.append("-" * 76)
    lines.append("本轮阈值（可用环境变量覆盖，见文件开头说明）：")
    lines.append(
        f"  P99 <= {THRESHOLDS['p99_ms']:.2f} ms      "
        f"P50 <= {THRESHOLDS['p50_ms']:.2f} ms      "
        f"失败率 < {THRESHOLDS['max_failure_ratio'] * 100:.3f}%"
    )
    lines.append(
        f"  跳转 QPS >= {THRESHOLDS['redirect_qps']:,.0f}      "
        f"创建 QPS >= {THRESHOLDS['shorten_qps']:,.0f}      "
        f"最少请求数 >= {THRESHOLDS['min_requests']}"
    )
    lines.append("-" * 76)
    if problems:
        lines.append(f"判定结果：[未达标] 有 {len(problems)} 项不符合目标：")
        for index, problem in enumerate(problems, 1):
            lines.append(f"  {index}. {problem}")
    else:
        lines.append("判定结果：[达标] 所有阈值都通过了。")
    lines.append("=" * 76)
    return "\n".join(lines)


# --------------------------- CSV 结果文件的解析 ---------------------------


def resolve_stats_csv_path(csv_prefix: str) -> str:
    """定位结果文件。

    用户可能给的是前缀（/tmp/mininet_perf），也可能直接把 *_stats.csv 的路径丢进来，
    两种都认。
    """
    if csv_prefix.lower().endswith(".csv"):
        return csv_prefix
    return csv_prefix + "_stats.csv"


def _cell(row: dict, column: str, label: str) -> str:
    return (row.get(column) or "").strip()


def to_int(row: dict, column: str, label: str) -> int:
    """把某一列读成整数；读到脏数据时给中文报错，而不是抛一堆看不懂的栈。"""
    raw = _cell(row, column, label)
    if raw in ("", "-", "N/A"):
        return 0
    try:
        return int(float(raw))
    except ValueError:
        raise CsvFormatError(
            f"CSV 里「{label}」这一行的「{column}」列不是数字：{raw!r}（文件可能被手工改坏了）"
        ) from None


def to_float_value(row: dict, column: str, label: str) -> float:
    """把某一列读成小数；说明同 to_int。"""
    raw = _cell(row, column, label)
    if raw in ("", "-", "N/A"):
        return 0.0
    try:
        return float(raw)
    except ValueError:
        raise CsvFormatError(
            f"CSV 里「{label}」这一行的「{column}」列不是数字：{raw!r}（文件可能被手工改坏了）"
        ) from None


def is_total_row(name: str, row_type: str) -> bool:
    """判断这一行是不是"总计"行。

    locust 各版本写法不完全一致：有的把总计行写成 Name=Aggregated，
    有的写成 Type=Aggregated 而 Name 为空，这里几种都认，别因为换个版本就读错总数。
    """
    if name == AGGREGATED_NAME or row_type == AGGREGATED_NAME:
        return True
    return not name and not row_type


def row_to_stat(row: dict, label: str) -> dict:
    """把 CSV 的一行转成跟我们统一格式一致的字典。"""
    requests = to_int(row, "Request Count", label)
    failures = to_int(row, "Failure Count", label)
    return {
        "requests": requests,
        "failures": failures,
        "failure_ratio": (failures / requests) if requests else 0.0,
        "qps": to_float_value(row, "Requests/s", label),
        "p50_ms": to_float_value(row, "50%", label),
        "p99_ms": to_float_value(row, "99%", label),
    }


def read_stats_csv(csv_prefix: str) -> dict:
    """读 locust 落盘的 <前缀>_stats.csv，转成 check_thresholds 要的规整字典。

    CSV 表头是 locust 固定写死的：
        Type,Name,Request Count,Failure Count,Median Response Time,Average Response Time,
        Min,Max,Average Content Size,Requests/s,Failures/s,50%,66%,75%,80%,90%,95%,98%,
        99%,99.9%,99.99%,100%
    每个聚合名一行，最后一行是总计（Aggregated）。
    """
    path = resolve_stats_csv_path(csv_prefix)
    if not os.path.isfile(path):
        raise CsvFormatError(
            f"找不到 CSV 结果文件：{path}\n"
            f"    提示：先跑一次压测生成结果，或者用 --csv-prefix 指到已有结果的路径上。"
        )

    try:
        handle = open(path, "r", encoding="utf-8-sig", newline="")
    except OSError as exc:
        raise CsvFormatError(f"打不开 CSV 结果文件：{path}（{exc}）") from exc

    groups = {}
    total = None
    with handle:
        reader = csv.DictReader(handle)
        headers = [(h or "").strip() for h in (reader.fieldnames or [])]
        if not headers:
            raise CsvFormatError(f"CSV 文件是空的、或者没有表头：{path}")
        missing = [column for column in CSV_COLUMNS_NEEDED if column not in headers]
        if missing:
            raise CsvFormatError(
                f"CSV 表头缺少这些列：{'、'.join(missing)}\n"
                f"    实际表头：{','.join(headers)}\n"
                f"    文件：{path}\n"
                f"    提示：这个文件多半不是 locust 的 *_stats.csv"
                f"（别把 *_failures.csv 或 *_stats_history.csv 传进来）。"
            )

        for raw in reader:
            row = {(k or "").strip(): (v or "").strip() for k, v in raw.items()}
            if not any(row.values()):
                continue  # 空行，跳过
            name = row.get("Name", "")
            row_type = row.get("Type", "")
            if is_total_row(name, row_type):
                total = row_to_stat(row, AGGREGATED_NAME)
            elif name:
                groups[name] = row_to_stat(row, name)

    if total is None:
        # 少见情况：CSV 里没有总计行（被裁剪过 / 版本差异），那就自己合并一份
        total = merge_stats(list(groups.values()))
    return {"total": total, "groups": groups}


# ===========================================================================
# 三、Locust 用户行为（只有装了 locust 时才定义）
# ===========================================================================
if LOCUST_AVAILABLE:

    class MininetUser(HttpUser):
        """一个虚拟用户 = 一个真实用户的"替身"。

        进场先给自己领一条短链（on_start），之后就在"跳转 / 创建 / 统计"之间随机晃。
        三个任务的权重是 6 : 3 : 1，对应 60% / 30% / 10% 的流量配比。
        只保留这一个 User 类就够了，locust 里不需要再写别的（weight 也不用设）。
        """

        host = HOST

        # 压满模式：两次请求之间不等待，用来测"极限吞吐"。
        # 想模拟真实用户的节奏（每次操作之间会思考、会停顿），把它改成
        # between(0.1, 0.5)——每个用户每次请求之间歇 0.1~0.5 秒，
        # 那时的 QPS 才更接近线上真实表现。
        wait_time = between(0, 0)

        def on_start(self):
            """每个虚拟用户进场时：先创建一条只属于自己的短链。

            为什么 URL 里要带"用户编号 + 随机串"：
            服务端对同一个 URL 会返回同一个短码（去重）。如果 1000 个用户都用同一个 URL，
            那测的其实是一条"命中已有记录"的读路径，而不是真实的创建路径。
            带上随机串，每个用户都有自己的短链，彼此互不干扰。
            """
            self.code = None
            self.own_url = (
                f"https://example.com/perf/user-{next(USER_SEQ)}-{uuid.uuid4().hex[:12]}"
            )

            with self.client.post(
                API_SHORTEN,
                json={"url": self.own_url},
                name=NAME_SHORTEN,
                catch_response=True,
            ) as resp:
                problem = judge_response(resp, (201,), "创建短链(on_start)")
                if problem:
                    resp.failure(problem)
                    return  # self.code 保持 None，后面的任务会自动跳过
                data = safe_json(resp)
                code = data.get("code") if isinstance(data, dict) else None
                if not isinstance(code, str) or not CODE_PATTERN.match(code):
                    resp.failure(
                        f"创建短链(on_start)：返回的 code 不合法：{code!r}，"
                        f"响应体前 200 字符：{body_preview(resp)}"
                    )
                    return
                resp.success()
                self.code = code

        @task(TASK_WEIGHT_REDIRECT)
        def redirect(self):
            """60% 的流量：拿自己的短码去跳转，期望 302 且 Location 指向自己那条 URL。"""
            if not self.code:
                # on_start 没拿到短码（服务有问题），这个任务就空转。
                # 不硬发请求的原因：否则错误统计会被一堆"假失败"灌满，分不清到底谁的问题。
                return

            with self.client.get(
                f"/{self.code}",
                name=NAME_REDIRECT,      # 聚合名，不然报告里会出现几千行
                allow_redirects=False,   # 只要 302 这个响应本身，别真跳到 example.com 去
                catch_response=True,
            ) as resp:
                problem = judge_response(resp, (302,), "跳转")
                if problem:
                    resp.failure(problem)
                    return
                location = resp.headers.get("Location", "")
                if location != self.own_url:
                    resp.failure(
                        f"跳转：Location 不对，期望 {self.own_url}，实际 {location!r}，"
                        f"响应体前 200 字符：{body_preview(resp)}"
                    )
                    return
                resp.success()

        @task(TASK_WEIGHT_SHORTEN)
        def shorten(self):
            """30% 的流量：创建短链。

            有 30% 的概率用"自己那条已经存在的 URL"再建一次，专门验证接口契约里的
            "同一个 URL 再创建会返回同一个短码"（顺带把去重逻辑也压一压）；
            剩下 70% 用全新的 URL，压真正的"新建"路径。
            """
            # 注意这里要求 self.code 也在（on_start 真的成功了），否则拿什么去比对返回的短码
            if self.own_url and self.code and random.random() < 0.3:
                url = self.own_url
                expect_code = self.code  # 期望拿回同一个短码
            else:
                url = f"https://example.com/perf/new-{uuid.uuid4().hex}"
                expect_code = None

            with self.client.post(
                API_SHORTEN,
                json={"url": url},
                name=NAME_SHORTEN,
                catch_response=True,
            ) as resp:
                problem = judge_response(resp, (201,), "创建短链")
                if problem:
                    resp.failure(problem)
                    return
                data = safe_json(resp)
                code = data.get("code") if isinstance(data, dict) else None
                if not isinstance(code, str) or not CODE_PATTERN.match(code):
                    resp.failure(
                        f"创建短链：返回的 code 不合法：{code!r}，"
                        f"响应体前 200 字符：{body_preview(resp)}"
                    )
                    return
                if expect_code is not None and code != expect_code:
                    resp.failure(
                        f"创建短链：同一个 URL 应该返回同一个 code，"
                        f"期望 {expect_code}，实际 {code}"
                    )
                    return
                resp.success()

        @task(TASK_WEIGHT_STATS)
        def stats(self):
            """10% 的流量：查统计，顺便断言 hits 是个非负整数。"""
            if not self.code:
                return

            with self.client.get(
                NAME_PREFIX_STATS + self.code,
                name=NAME_STATS,
                catch_response=True,
            ) as resp:
                problem = judge_response(resp, (200,), "统计")
                if problem:
                    resp.failure(problem)
                    return
                data = safe_json(resp)
                if not isinstance(data, dict):
                    resp.failure(
                        f"统计：响应不是 JSON 对象，响应体前 200 字符：{body_preview(resp)}"
                    )
                    return
                hits = data.get("hits")
                # 注意：Python 里 True 也是 int（bool 是 int 的子类），先把它排掉，
                # 不然服务端返回 true 会被当成 1 蒙混过关。
                if isinstance(hits, bool) or not isinstance(hits, int) or hits < 0:
                    resp.failure(
                        f"统计：hits 应该是 >= 0 的整数，实际 {hits!r}，"
                        f"响应体前 200 字符：{body_preview(resp)}"
                    )
                    return
                resp.success()


# ===========================================================================
# 四、Locust 事件钩子：一轮压测结束时的"验收"
# ===========================================================================
if LOCUST_AVAILABLE:

    @events.quitting.add_listener
    def on_quitting(environment, **kwargs):
        """压测结束时自动算账：打印中文汇总表，不达标就把退出码设成 1（CI 因此变红）。"""
        # environment.ready 只有在 locust 真正跑起来之后才是 True。
        # 比如别人只是 import 这个文件、或者启动一半就被取消，这时统计是空的，
        # 直接返回，免得拿半截数据误报。
        if not getattr(environment, "ready", False):
            return

        try:
            summary = summarize_stats(environment.stats)
        except Exception as exc:  # 汇总本身出错也不能把 locust 弄崩
            print(f"[校验] 汇总统计时出错，跳过阈值判定：{exc}", file=sys.stderr)
            return

        total_requests = int((summary.get("total") or {}).get("requests", 0) or 0)
        if total_requests <= 0:
            # 一个请求都没发出去，说明这轮等于没测：必须算失败，不能让 CI 假装通过
            print(
                "[校验] 这一轮一个请求都没发出去（服务没起来？地址写错了？），判定为不通过。",
                file=sys.stderr,
            )
            environment.process_exit_code = 1
            return

        problems = check_thresholds(summary)
        print(format_report(summary, problems))
        if problems:
            print(f"[校验] 有 {len(problems)} 项没达标，退出码设为 1（CI 会变红）。")
            environment.process_exit_code = 1
        else:
            print("[校验] 全部达标。")
            environment.process_exit_code = 0


# ===========================================================================
# 五、命令行入口（独立运行：压测 + 校验，或者只校验 CSV）
# ===========================================================================


def parse_args(argv=None) -> argparse.Namespace:
    """解析命令行参数，全部带中文帮助。"""
    parser = argparse.ArgumentParser(
        prog="locustfile.py",
        description=(
            "mini-net 性能压测脚本。既能被 locust 加载着跑，也能单独运行："
            "单独运行 = 帮你调 locust 压一轮 + 自动校验阈值；加 --check-only = 只校验已有的 CSV。"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "常用例子：\n"
            "  # 压一轮 1000 并发、每秒拉起 100 个用户、跑 60 秒，并自动校验阈值\n"
            "  python3 tests/perf/locustfile.py --users 1000 --spawn-rate 100 --run-time 60s\n"
            "\n"
            "  # 只校验上一轮跑出来的 CSV（CI 里分两步跑时用）\n"
            "  python3 tests/perf/locustfile.py --check-only --csv-prefix /tmp/mininet_perf\n"
            "\n"
            "  # 本机 4 核自测：把期望值调低，只验证链路通不通\n"
            "  PERF_REDIRECT_QPS=8000 PERF_SHORTEN_QPS=4000 \\\n"
            "      python3 tests/perf/locustfile.py --users 200 --run-time 30s\n"
        ),
    )
    parser.add_argument(
        "--users",
        type=int,
        default=1000,
        help="并发虚拟用户数（默认 1000，对应项目文档里的 1000 并发场景）",
    )
    parser.add_argument(
        "--spawn-rate",
        type=float,
        default=100.0,
        help="每秒拉起多少个虚拟用户（默认 100；别设太大，服务需要时间热身）",
    )
    parser.add_argument(
        "--run-time",
        default="60s",
        help="压测时长，写 30s / 2m / 1h 都行（默认 60s）",
    )
    parser.add_argument(
        "--host",
        default=HOST,
        help=f"被测服务地址（默认 {HOST}，也可以用环境变量 MININET_HOST 覆盖）",
    )
    parser.add_argument(
        "--csv-prefix",
        default=None,
        help=(
            "CSV 结果文件的前缀。压测时会生成 <前缀>_stats.csv 等文件；"
            "校验时就读这个前缀对应的文件（也可以直接给 .csv 文件的完整路径）。"
            "默认放到系统临时目录下：<临时目录>/mininet_perf"
        ),
    )
    parser.add_argument(
        "--check-only",
        action="store_true",
        help="不跑压测，只读取已有的 CSV 结果做阈值校验（配合 --csv-prefix 使用）",
    )
    parser.add_argument(
        "--processes",
        type=int,
        default=1,
        help=(
            "用几个 locust 进程来发压（默认 1）。压测机核多时可以调大，"
            "比如 8 核机器用 --processes 4，能把发包能力顶上去，压出更真实的数字"
        ),
    )
    return parser.parse_args(argv)


def build_locust_command(args, csv_prefix: str) -> list:
    """拼出要交给 subprocess 执行的 locust 命令。"""
    command = [
        "--headless",                      # 无界面模式
        "-f", os.path.abspath(__file__),   # 本文件自己就是 locust 脚本
        "-u", str(args.users),             # 并发用户数
        "-r", str(args.spawn_rate),        # 每秒拉起多少用户
        "-t", str(args.run_time),          # 跑多久
        "--host", args.host,
        "--csv", csv_prefix,               # 把结果落成 CSV，我们待会儿要解析它
        "--only-summary",                  # 别刷屏，最后只打一张汇总表
    ]
    if args.processes and args.processes > 1:
        command += ["--processes", str(args.processes)]

    locust_exe = shutil.which("locust")
    if locust_exe:
        return [locust_exe] + command
    # PATH 里没有 locust 命令时，退回"用当前这个 Python 去跑 locust 模块"。
    # 这两种情况很常见：pip 装到用户目录、或者脚本目录不在 PATH 里。
    return [sys.executable, "-m", "locust"] + command


def run_locust(args, csv_prefix: str) -> int:
    """调 locust 压一轮，返回它的退出码（正常是 0）。"""
    command = build_locust_command(args, csv_prefix)
    print(f"[压测] 执行命令：{' '.join(command)}")
    print(
        f"[压测] 并发 {args.users} 个用户，每秒拉起 {args.spawn_rate} 个，"
        f"跑 {args.run_time}，目标服务 {args.host}"
    )
    started = time.time()
    try:
        completed = subprocess.run(command, check=False)
    except FileNotFoundError:
        print(
            "[错误] 找不到 locust 命令，也没法用 python -m locust 启动它。\n"
            "    解决办法：先装依赖（只需一次）\n"
            "        pip install locust\n"
            "    装完确认一下：locust --version",
            file=sys.stderr,
        )
        return 2
    except KeyboardInterrupt:
        print("\n[压测] 被你用 Ctrl+C 中断了，不再做阈值判定。", file=sys.stderr)
        return 130
    print(
        f"[压测] locust 结束，退出码 {completed.returncode}，"
        f"总耗时 {time.time() - started:.1f} 秒"
    )
    return completed.returncode


def main(argv=None) -> int:
    """独立运行的入口：压测 + 校验，或者只校验 CSV。"""
    args = parse_args(argv)

    # 默认把结果放到系统临时目录：Linux 上就是 /tmp/mininet_perf
    csv_prefix = args.csv_prefix or os.path.join(tempfile.gettempdir(), "mininet_perf")

    if not args.check_only and not LOCUST_AVAILABLE:
        print(
            "[错误] 当前 Python 里没有装 locust，没法跑压测。\n"
            f"    具体报错：{LOCUST_IMPORT_ERROR}\n"
            "    解决办法（只需一次）：\n"
            "        pip install locust\n"
            "    如果只想校验已有的 CSV 结果、不跑压测，可以加 --check-only：\n"
            f"        python3 {os.path.basename(__file__)} --check-only --csv-prefix {csv_prefix}",
            file=sys.stderr,
        )
        return 2

    locust_exit_code = 0
    if args.check_only:
        print(f"[校验] 只校验模式：读取 {resolve_stats_csv_path(csv_prefix)}")
    else:
        if args.users <= 0:
            print(f"[错误] --users 要大于 0，现在是 {args.users}", file=sys.stderr)
            return 2
        if args.spawn_rate <= 0:
            print(f"[错误] --spawn-rate 要大于 0，现在是 {args.spawn_rate}", file=sys.stderr)
            return 2
        locust_exit_code = run_locust(args, csv_prefix)
        if locust_exit_code == 130:
            return 130  # 用户主动中断，不再判定

    try:
        summary = read_stats_csv(csv_prefix)
    except CsvFormatError as exc:
        print(f"[错误] {exc}", file=sys.stderr)
        return 2

    problems = check_thresholds(summary)
    print(format_report(summary, problems))

    if problems:
        print(f"[结果] 有 {len(problems)} 项没达标，退出码 1。")
        return 1
    print("[结果] 全部达标。")
    # 阈值都过了，但 locust 自己可能因为别的原因返回非 0（比如压测中途报错），如实传递
    return 0 if locust_exit_code in (0, None) else locust_exit_code


# 这一句很重要：locust 是以 import 的方式加载本文件的，
# 如果把这段逻辑放在顶层，locust 自己还没启动就会被我们抢着执行，直接坏掉。
if __name__ == "__main__":
    sys.exit(main())
