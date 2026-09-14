# -*- coding: utf-8 -*-
"""
mini-net 短链服务 —— HTTP 接口自动化测试

被测对象：docs/API.md 里定义的 5 个端点
  1. POST /api/shorten   创建短链
  2. GET  /{code}        跳转
  3. GET  /api/stats/{code}  统计
  4. GET  /healthz       健康检查
  5. GET  /metrics       指标

怎么跑：
  # 先启动服务（默认 8080），然后：
  python -m pytest tests/api -v
  # 指向别的机器：
  python -m pytest tests/api -v --base-url http://10.0.0.5:8080
  # 把慢用例也跑上（要等 5 秒以上才会返回 408 的那条）：
  python -m pytest tests/api -v --run-slow

小提示：每条用例开头都会 print 一行中文日志。
这样当你用 --base-url 指向远端服务、用例失败时，能马上知道是"哪一步"炸的。
"""

from __future__ import annotations

import concurrent.futures
import http.client
import json
import socket
import time
import uuid
from typing import List, Tuple

import pytest
import requests

from conftest import (
    CODE_RE,
    ServerAddr,
    allure_step,
    assert_error_body,
    assert_raw_error,
    build_raw_request,
    parse_http_response,
    parse_metrics,
    raw_http,
    raw_http_until_close,
    read_one_response,
)

# 8 KiB = 8192 字节，契约里的请求体 / 请求头上限
LIMIT_8K = 8 * 1024

pytestmark = [pytest.mark.api]


# ---------------------------------------------------------------------------
# 用例内部用的小工具
# ---------------------------------------------------------------------------
def build_url_of_length(total: int, prefix: str = "https://example.com/") -> str:
    """造一个**长度精确等于 total** 的合法 https URL（尾部带随机串，保证不重复）。"""
    token = uuid.uuid4().hex[:16]
    filler_len = total - len(prefix) - len(token)
    assert filler_len >= 1, "prefix 太长了，造不出这么短的 URL"
    return prefix + ("p" * filler_len) + token


def socket_connect(server_addr: ServerAddr, timeout: float = 5.0):
    """开一条到服务的裸 TCP 连接（调用方负责关闭）。

    为什么需要它？因为有的用例要"占着一条连接"不放，
    或者要发 requests 发不出来的畸形请求。
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect((server_addr.host, server_addr.port))
    except OSError as exc:
        sock.close()
        pytest.fail(f"连接 {server_addr.host}:{server_addr.port} 失败：{exc}（服务没起来吗？）")
    return sock


def get_hits(api, code: str) -> int:
    """查某个短码当前被访问了多少次。"""
    resp = api.stats(code)
    assert resp.status_code == 200, f"查统计失败：期望 200，实际 {resp.status_code}；响应={resp.text[:200]!r}"
    return resp.json()["hits"]


def shorten_until_mixed_case(api, shorten, unique_url, attempts: int = 40) -> str:
    """一直到拿到的短码里"含有小写字母"为止。

    为什么要这么绕？因为大小写敏感这条用例，需要拿"全部转成大写"的短码去查，
    如果短码本身就是全大写（比如 ABC123），转成大写还是它自己，这条用例就测不出东西了。
    """
    for _ in range(attempts):
        code = shorten(unique_url("case"))
        if code != code.upper():
            return code
    pytest.fail(f"连续创建 {attempts} 次，短码里都没有小写字母，没法验证大小写敏感（服务端短码生成规则可能有问题）")


def post_shorten_in_thread(base_url: str, timeout: float, url: str) -> Tuple[int, dict]:
    """线程里干活的函数：自己开一个 requests.Session 去创建短链。

    **重点**：requests.Session 不是线程安全的（内部连接池共用状态），
    所以每个线程必须用自己的 Session，否则会出现"串台"的诡异失败。
    """
    session = requests.Session()
    try:
        resp = session.post(
            base_url + "/api/shorten",
            json={"url": url},
            timeout=timeout,
            allow_redirects=False,
        )
        return resp.status_code, resp.json()
    finally:
        session.close()


def get_redirect_in_thread(base_url: str, timeout: float, code: str) -> int:
    """线程里干活的函数：自己开一个 Session 去访问短链，返回状态码。"""
    session = requests.Session()
    try:
        resp = session.get(base_url + "/" + code, timeout=timeout, allow_redirects=False)
        return resp.status_code
    finally:
        session.close()


# ===========================================================================
# 一、创建短链  POST /api/shorten
# ===========================================================================
class TestCreateShorten:
    """创建短链相关用例（用例编号 C-xx）。"""

    @pytest.mark.smoke
    @pytest.mark.parametrize(
        "url",
        [
            "https://example.com/very/long/path?x=1",
            "http://example.com/plain",
            "https://example.com/a/b/c/d/e?q=hello&n=42#section-2",
        ],
        ids=["https-protocol", "http-protocol", "path-with-query"],
    )
    def test_c01_create_ok(self, api, url):
        """用例 C-01：正常创建一个短链 → 201，且 code/short/url 三个字段都齐全、取值正确。

        这是最核心的一条：返回的 short 必须等于「服务地址 + / + code」，
        url 必须原样回显（不能多一个斜杠、不能少一段 query）。
        """
        print(f"[C-01] 创建短链：{url}")
        with allure_step("POST /api/shorten 创建短链"):
            resp = api.post_json("/api/shorten", {"url": url})

        assert resp.status_code == 201, f"期望 201 Created，实际 {resp.status_code}；响应={resp.text[:300]!r}"
        body = resp.json()

        for field in ("code", "short", "url"):
            assert field in body, f"响应里缺少 {field} 字段：{body!r}"

        code = body["code"]
        assert CODE_RE.match(code), f"短码不符合 ^[0-9a-zA-Z]{{1,8}}$ 规则：{code!r}"
        assert body["short"] == api.base_url + "/" + code, (
            f"short 字段应该是「服务地址 + / + 短码」；期望 {api.base_url + '/' + code!r}，实际 {body['short']!r}"
        )
        assert body["url"] == url, f"url 应该原样回显；期望 {url!r}，实际 {body['url']!r}"

    def test_c02_content_type_is_json(self, api, unique_url):
        """用例 C-02：创建接口的响应 Content-Type 必须是 application/json。

        如果 Content-Type 不对，客户端（比如浏览器、SDK）就不会把响应当 JSON 解析，
        接口就算能返回数据也等于白搭。
        """
        print("[C-02] 检查创建接口的响应 Content-Type")
        resp = api.post_json("/api/shorten", {"url": unique_url("c02")})
        assert resp.status_code == 201, f"期望 201，实际 {resp.status_code}；响应={resp.text[:200]!r}"
        content_type = resp.headers.get("Content-Type", "")
        assert "application/json" in content_type.lower(), f"Content-Type 应该是 application/json，实际是 {content_type!r}"

    def test_c03_same_url_returns_same_code(self, api, unique_url):
        """用例 C-03：同一个 url 连续创建 3 次 → 三次拿到的短码完全相同，状态码都是 201。

        契约规定"同一个 url 复用同一个短码"，这是幂等性。
        如果每次都发新码，数据库会很快被同一个网址撑爆。
        """
        print("[C-03] 同一个 url 连续创建 3 次，验证幂等")
        url = unique_url("c03")
        codes: List[str] = []
        statuses: List[int] = []
        for index in range(3):
            resp = api.post_json("/api/shorten", {"url": url})
            statuses.append(resp.status_code)
            if resp.status_code == 201:
                codes.append(resp.json()["code"])

        assert statuses == [201, 201, 201], f"三次创建的状态码都应该是 201，实际是 {statuses}"
        assert len(set(codes)) == 1, f"同一个 url 三次创建应该得到同一个短码，实际拿到的是 {codes}"

    @pytest.mark.parametrize(
        "body",
        [
            {},  # url 字段压根没有
            {"url": None},  # url 是 null
            {"url": 12345},  # url 是数字
            {"url": ["https://example.com/a"]},  # url 是数组
            {"url": {"nested": "https://example.com/a"}},  # url 是对象
            {"url": ""},  # url 是空字符串
            {"url": True},  # url 是布尔值
            {"url": "ftp://example.com/a"},  # 协议不对
            {"url": "javascript:alert(1)"},  # 伪协议（安全上必须拦掉）
            {"url": "file:///etc/passwd"},  # 本地文件协议（安全上必须拦掉）
            {"url": "example.com/x"},  # 根本没写协议头
        ],
        ids=[
            "url缺失",
            "url为null",
            "url为数字",
            "url为数组",
            "url为对象",
            "url为空字符串",
            "url为布尔",
            "ftp协议",
            "javascript伪协议",
            "file协议",
            "缺少scheme",
        ],
    )
    def test_c04_invalid_url(self, api, body):
        """用例 C-04：各种非法 url → 400 且 error == invalid_url。

        这里用参数化一次覆盖 11 种坏输入：字段缺失、类型不对、内容不合法。
        为什么要挨个测？因为"少写一个判断"是这类接口最常见的 bug，
        比如只判断了字符串长度，忘了判断类型，就会把数字当网址存进去。
        """
        print(f"[C-04] 提交非法 url：{body!r}")
        resp = api.post_json("/api/shorten", body)
        assert_error_body(resp, 400, "invalid_url")

    def test_c05a_url_length_2048_is_ok(self, api):
        """用例 C-05a：url 长度**刚好 2048** → 201（边界值应当通过）。

        边界值测试的思路：规定是"不超过 2048"，那 2048 就必须通过、2049 就必须拒绝。
        程序员最容易把边界写错（写成 > 而不是 >=，或者 off-by-one）。
        """
        url = build_url_of_length(2048)
        print(f"[C-05a] 提交长度恰好 2048 的 url（实际长度 {len(url)}）")
        resp = api.post_json("/api/shorten", {"url": url})
        assert resp.status_code == 201, f"长度 2048 应当被接受，实际 {resp.status_code}；响应={resp.text[:300]!r}"
        assert resp.json()["url"] == url, "长度 2048 的 url 必须原样回显"

    def test_c05b_url_length_2049_is_rejected(self, api):
        """用例 C-05b：url 长度 2049（超一位）→ 400 invalid_url。"""
        url = build_url_of_length(2049)
        print(f"[C-05b] 提交长度 2049 的 url（实际长度 {len(url)}）")
        resp = api.post_json("/api/shorten", {"url": url})
        assert_error_body(resp, 400, "invalid_url")

    @pytest.mark.parametrize(
        "url",
        [
            "https://example.com/very/long/path?x=1&y=2#fragment",
            "https://Example.COM:8443/Mixed/Case/PATH",
            "https://例子.测试/路径/中文?关键字=值",
            "http://192.168.1.100:8080/a/b/c?d=1",
            "https://example.com/search?q=" + ("x" * 1500),
        ],
        ids=["query加fragment", "带端口大小写混合host", "中文unicode", "IP地址", "超长query"],
    )
    def test_c06_special_url_echo(self, api, url):
        """用例 C-06：特殊长相的 url 都要能创建成功，并且**原样**保存/回显。

        这里挑的都是真实世界里常见的地址：带 #锚点、带端口、中文域名、
        IP 直连、超长查询串。原样回显很重要——
        只要服务端做了转义或截断，跳转过去就会变成另一个页面。
        """
        print(f"[C-06] 特殊 url（{len(url)} 字符）")
        resp = api.post_json("/api/shorten", {"url": url})
        assert resp.status_code == 201, (
            f"这个 url 应当被接受，实际 {resp.status_code}；url={url[:80]!r}...；响应={resp.text[:300]!r}"
        )
        body = resp.json()
        assert body["url"] == url, f"url 必须原样回显；期望 {url!r}，实际 {body['url']!r}"
        assert CODE_RE.match(body["code"]), f"短码不合法：{body['code']!r}"

    @pytest.mark.parametrize(
        "raw_body",
        [
            b"{not json",
            b"",
            b"   \r\n  ",
            b"[1,2,3]",
            b'"just a string"',
            b'{"url": "https://example.com/x"',
        ],
        ids=["括号不配对", "空body", "只有空白", "顶层是数组", "顶层是字符串", "json被截断"],
    )
    def test_c07_malformed_body(self, api, raw_body):
        """用例 C-07：请求体不是合法 JSON（或不是 JSON 对象）→ 400。

        契约没有给这种"语法就错了"的情况单独定 error 取值（表里只有 invalid_url 等），
        所以这里只严格要求：状态码 400 + 统一错误格式里的 error/detail 都在。
        """
        print(f"[C-07] 提交坏掉的请求体：{raw_body!r}")
        with allure_step("POST /api/shorten（坏 JSON）"):
            resp = api.post_json("/api/shorten", raw_body=raw_body)
        assert_error_body(resp, 400)

    @pytest.mark.parametrize(
        "content_type",
        ["text/plain", None],
        ids=["text-plain", "完全没有Content-Type"],
    )
    def test_c08_content_type_not_json(self, api, unique_url, content_type):
        """用例 C-08：Content-Type 不是 application/json 时，服务端不能"看不懂却硬存"。

        契约没有明确写这种情况该怎么办，所以宽容处理：
        严格拒绝（400）和宽容接受（201）都算通过；但如果返回了 5xx 就是真的挂了。
        """
        url = unique_url("c08")
        print(f"[C-08] 用 {content_type!r} 这种 Content-Type 发 JSON 体")
        payload = json.dumps({"url": url}).encode("utf-8")
        if content_type is None:
            resp = api.request("POST", "/api/shorten", data=payload)
        else:
            resp = api.request("POST", "/api/shorten", data=payload, headers={"Content-Type": content_type})

        assert resp.status_code in (200, 201, 400), (
            f"契约未明确这种写法，但至少不该是 {resp.status_code}（服务端异常）；响应={resp.text[:300]!r}"
        )
        if resp.status_code == 400:
            assert_error_body(resp, 400)
            assert resp.json()["error"] in ("invalid_url", "unsupported_media_type", "bad_request"), (
                f"400 的 error 取值超出了合理范围：{resp.json()!r}"
            )


# ===========================================================================
# 二、跳转  GET /{code}
# ===========================================================================
class TestRedirect:
    """跳转相关用例（用例编号 R-xx）。"""

    @pytest.mark.smoke
    def test_r01_redirect_302(self, api, shorten, unique_url):
        """用例 R-01：创建后访问短链 → 302，Location 精确等于原 url，Cache-Control 是 no-store。

        两个关键点：
        1) Location 必须一模一样，否则用户会被带到别的页面去。
        2) Cache-Control: no-store 是告诉大家"别缓存这个跳转"，
           否则浏览器记住了跳转结果，统计出来的 hits 就不准了。
        3) 我们已经把 allow_redirects 关掉，所以拿到的是 302 本身，
           不是被自动跟到目标网址后的 200（resp.history 为空可以佐证）。
        """
        url = unique_url("r01")
        print(f"[R-01] 创建短链后访问它，期望 302 且 Location 精确等于 {url}")
        code = shorten(url)

        with allure_step("GET /{code} 期望 302"):
            resp = api.get("/" + code)

        assert resp.status_code == 302, f"期望 302 Found，实际 {resp.status_code}；响应={resp.text[:200]!r}"
        assert resp.headers.get("Location") == url, (
            f"Location 必须精确等于原 url；期望 {url!r}，实际 {resp.headers.get('Location')!r}"
        )
        cache_control = resp.headers.get("Cache-Control", "")
        assert cache_control.lower() == "no-store", f"Cache-Control 应该是 no-store，实际是 {cache_control!r}"
        assert resp.history == [], "不应该跟随重定向（allow_redirects 必须为 False），否则测的不是 302 本身"

    def test_r02_hits_increment_exactly(self, api, shorten, unique_url):
        """用例 R-02：连续跳转 3 次 → 统计里的 hits 精确地从 0 涨到 3。

        用"新造"的短码来测，这样起点一定是 0，涨多少就是多少，能精确断言。
        如果服务端把计数写漏或写重，这条用例会立刻发现。
        """
        url = unique_url("r02")
        print(f"[R-02] 用一个全新的短码跳 3 次，hits 应当从 0 精确涨到 3；url={url}")
        code = shorten(url)
        before = get_hits(api, code)

        for index in range(3):
            resp = api.get("/" + code)
            assert resp.status_code == 302, f"第 {index + 1} 次跳转应该是 302，实际 {resp.status_code}"

        after = get_hits(api, code)
        assert before == 0, f"新短码的 hits 起点应该是 0，实际是 {before}"
        assert after == 3, f"跳转 3 次后 hits 应该精确等于 3，实际是 {after}（计数可能漏加或多加）"

    @pytest.mark.parametrize(
        "code",
        ["zzzzzzzz", "QqQqQqQq", "00000000"],
        ids=["全小写8位", "大小写混合8位", "全数字8位"],
    )
    def test_r03_unknown_code_404(self, api, code):
        """用例 R-03：不存在的（但格式合法的）短码 → 404 not_found。

        注意：这里故意用"格式合法"的短码，因为格式非法的会走到 400 那条分支去。
        """
        print(f"[R-03] 访问不存在的短码 /{code}")
        resp = api.get("/" + code)
        assert_error_body(resp, 404, "not_found")

    @pytest.mark.parametrize(
        "code",
        ["ab$cd", "ab%20cd", "ab.cd"],
        ids=["美元符号", "百分号编码空格", "点号"],
    )
    def test_r04_illegal_code_chars(self, api, code):
        """用例 R-04：短码里含非法字符（$ %20 . 等）→ 400 invalid_code。

        为什么这属于安全测试？因为短码如果不做字符白名单校验，
        就可能被人拿去搞路径穿越（比如 /../../etc/passwd）或者注入奇怪的东西。
        """
        print(f"[R-04] 访问含非法字符的短码 /{code}")
        resp = api.get("/" + code)
        assert_error_body(resp, 400, "invalid_code")

    def test_r04b_illegal_code_with_raw_space(self, server_addr: ServerAddr):
        """用例 R-04b：短码里塞一个**真正的空格**（裸 socket 才能发出去）→ 400（或 404）。

        为什么用裸 socket？因为 requests 会把空格自动转义成 %20，
        我们想发"原汁原味"的坏请求，就只能自己拼字节。
        为什么宽容到允许 404？因为 `GET /ab cd HTTP/1.1` 这一行本身就不符合 HTTP 语法，
        服务端把它当成"路径 /ab、版本 cd"或者"路径 /ab cd"都说得通，契约没有规定到这么细。
        """
        payload = build_raw_request(
            "GET", "/ab cd", headers={"Host": f"{server_addr.host}:{server_addr.port}", "Connection": "close"}
        )
        print("[R-04b] 用裸 socket 发一个含空格短码的请求")
        data = raw_http(server_addr.host, server_addr.port, payload, read_timeout=3.0)
        status, _headers, _body = parse_http_response(data)
        assert status in (400, 404), f"应该是 400 或 404，实际 {status}；响应={data[:300]!r}"
        body = assert_raw_error(data, status)
        assert body["error"] in ("invalid_code", "not_found"), f"error 取值超出合理范围：{body!r}"

    @pytest.mark.parametrize(
        "code",
        ["abcdefghi", "abcdefghijkl", "ABCDEFGHIJKLMNOP"],
        ids=["9位", "12位", "16位"],
    )
    def test_r05_code_too_long(self, api, code):
        """用例 R-05：超过 8 位的短码 → 400 或 404，但 body 必须是统一错误格式。

        状态码为什么允许两种？因为"太长"既可以说是"格式非法"（400），
        也可以说是"这个码不存在"（404），契约里没把话说死，两种实现都合理。
        但无论哪种，body 都必须是 {"error": ..., "detail": ...}，不能吐 HTML 或者空响应。
        """
        print(f"[R-05] 访问超长短码（{len(code)} 位）")
        resp = api.get("/" + code)
        assert resp.status_code in (400, 404), (
            f"超长短码应该是 400 或 404，实际 {resp.status_code}；响应={resp.text[:200]!r}"
        )
        body = assert_error_body(resp, resp.status_code)
        assert body["error"] in ("invalid_code", "not_found"), f"error 取值超出合理范围：{body!r}"

    def test_r06_code_is_case_sensitive(self, api, shorten, unique_url):
        """用例 R-06：短码**大小写敏感** —— 小写码用全大写去查 → 404。

        为什么这条重要？如果服务端内部用"不区分大小写"的方式存短码，
        ABC 和 abc 会互相覆盖，用户拿到的短链就会跳到别人的页面上（严重的串号 bug）。
        """
        print("[R-06] 短码大小写敏感：先要到一个含小写字母的短码，再用全大写去查，期望 404")
        code = shorten_until_mixed_case(api, shorten, unique_url)
        upper = code.upper()
        assert upper != code, "前置条件：短码里必须含有小写字母"
        resp = api.get("/" + upper)
        assert_error_body(resp, 404, "not_found")


# ===========================================================================
# 三、统计  GET /api/stats/{code}
# ===========================================================================
class TestStats:
    """统计相关用例（用例编号 S-xx）。"""

    @pytest.mark.smoke
    def test_s01_stats_fields_and_types(self, api, shorten, unique_url):
        """用例 S-01：200，且 code/url/hits/created_at 四个字段的类型都对。

        这条专门盯"类型"：hits 必须是整数（不能是 "12" 这种字符串，
        否则前端算数会变成字符串拼接），created_at 必须是秒级 Unix 时间戳。
        """
        url = unique_url("s01")
        print(f"[S-01] 创建后查它的统计，检查字段齐全 + 类型正确；url={url}")
        code = shorten(url)

        with allure_step("GET /api/stats/{code}"):
            resp = api.stats(code)

        assert resp.status_code == 200, f"期望 200，实际 {resp.status_code}；响应={resp.text[:200]!r}"
        body = resp.json()
        for field in ("code", "url", "hits", "created_at"):
            assert field in body, f"统计响应缺少 {field} 字段：{body!r}"

        assert body["code"] == code, f"code 应该是 {code!r}，实际 {body['code']!r}"
        assert body["url"] == url, f"url 应该是 {url!r}，实际 {body['url']!r}"
        assert isinstance(body["hits"], int) and not isinstance(body["hits"], bool), (
            f"hits 应该是整数，实际是 {type(body['hits']).__name__}：{body['hits']!r}"
        )
        assert body["hits"] >= 0, f"hits 不该是负数：{body['hits']!r}"
        assert isinstance(body["created_at"], int) and not isinstance(body["created_at"], bool), (
            f"created_at 应该是整数时间戳，实际是 {type(body['created_at']).__name__}：{body['created_at']!r}"
        )

    def test_s02_created_at_is_reasonable(self, api, shorten, unique_url):
        """用例 S-02：created_at 落在合理区间内（> 2023-11-14，且不超过现在 + 60 秒）。

        为什么要留 60 秒的富余？因为服务端和测试机的时间可能有几秒误差，
        卡得太死会因为"时钟没对齐"而误报失败。
        """
        now = int(time.time())
        print(f"[S-02] 新建短码的 created_at 应当落在合理区间（当前时间戳 {now}）")
        code = shorten(unique_url("s02"))
        body = api.stats(code).json()
        created_at = body["created_at"]
        assert created_at > 1700000000, f"created_at 太小了，不像秒级时间戳：{created_at}"
        assert created_at <= now + 60, f"created_at 比当前时间还晚太多，时钟或单位可能有问题：{created_at} > {now}"
        assert created_at >= now - 3600, f"刚创建的短码，created_at 不该是一小时前：{created_at}"

    def test_s03_fresh_code_hits_is_zero(self, api, shorten, unique_url):
        """用例 S-03：全新短码的 hits 必须是 0（还没人访问过）。

        这是"S-01 类型正确"之外的独立性检查：如果新码一出生 hits 就不是 0，
        说明计数逻辑一开始就多加了（比如把创建请求也算成一次访问）。
        """
        print("[S-03] 全新短码还没被访问过，hits 必须是 0")
        code = shorten(unique_url("s03"))
        assert get_hits(api, code) == 0, "刚创建、还没被访问过的短码，hits 必须为 0"

    @pytest.mark.parametrize("code", ["zzzzzzzz", "NoSuchCd"], ids=["全小写8位", "混合大小写8位"])
    def test_s04_stats_unknown_code_404(self, api, code):
        """用例 S-04：查不存在的短码统计 → 404 not_found。

        注意这里用的短码**长度必须是合法的 8 位以内**：
        如果写成 NoSuchCode（10 位），服务端会判成"格式非法"返回 400，
        那就变成在测另一条分支了（400 invalid_code 由 R-05 负责）。
        """
        print(f"[S-04] 查不存在的短码统计 /api/stats/{code}")
        resp = api.stats(code)
        assert_error_body(resp, 404, "not_found")


# ===========================================================================
# 四、健康检查与指标
# ===========================================================================
class TestHealthAndMetrics:
    """健康检查和指标相关用例（用例编号 H-xx）。"""

    @pytest.mark.smoke
    def test_h01_healthz(self, api):
        """用例 H-01：GET /healthz → 200，body 恰好是 ok，Content-Type 是 text/plain。

        为什么要求 body "恰好是 ok"？因为容器编排工具（k8s 之类）就是靠这个字符串
        判断服务活没活的，多一个换行、多一个字母都可能让探针判失败。
        """
        print("[H-01] 健康检查 GET /healthz")
        with allure_step("GET /healthz"):
            resp = api.healthz()

        assert resp.status_code == 200, f"期望 200，实际 {resp.status_code}；响应={resp.text[:200]!r}"
        assert resp.content == b"ok", f"body 必须恰好是 ok，实际是 {resp.content!r}"
        content_type = resp.headers.get("Content-Type", "")
        assert "text/plain" in content_type.lower(), f"Content-Type 应该含 text/plain，实际是 {content_type!r}"

    def test_h02_metrics_has_all_keys(self, api):
        """用例 H-02：/metrics → 200，且 7 个约定指标键全都在，值都能当数字解析。

        这 7 个键是契约里点名的。少一个，监控面板上就会出现空白，
        而监控恰恰是"服务出问题时唯一能看的东西"。
        """
        print("[H-02] 检查 /metrics 的 7 个指标键")
        with allure_step("GET /metrics"):
            resp = api.metrics_raw()

        assert resp.status_code == 200, f"期望 200，实际 {resp.status_code}；响应={resp.text[:200]!r}"
        content_type = resp.headers.get("Content-Type", "")
        assert "text/plain" in content_type.lower(), f"Content-Type 应该含 text/plain，实际是 {content_type!r}"

        metrics = parse_metrics(resp.text)
        expected_keys = [
            "uptime_seconds",
            "alive_connections",
            "total_connections",
            "total_requests",
            "total_bytes_in",
            "shorten_total",
            "redirect_total",
        ]
        missing = [key for key in expected_keys if key not in metrics]
        assert not missing, f"缺少这些指标键：{missing}；服务实际返回的内容={resp.text[:500]!r}"
        for key in expected_keys:
            assert isinstance(metrics[key], float), f"{key} 的值解析不成数字：{metrics[key]!r}"

    def test_h03_total_requests_monotonic(self, api):
        """用例 H-03：/metrics 采样两次，total_requests 只会涨不会跌。

        计数器必须单调递增。如果它变小了，说明要么是竞态导致写丢，
        要么是服务重启了（重启后应该清零，但那就意味着 uptime_seconds 也会归零）。
        """
        print("[H-03] 采样两次 /metrics，验证 total_requests 不减少")
        first = api.metrics()
        # 中间故意制造几次请求，让计数器有机会往上走
        api.healthz()
        api.healthz()
        second = api.metrics()

        assert "total_requests" in first and "total_requests" in second, f"采样结果缺字段：{first} / {second}"
        assert second["total_requests"] >= first["total_requests"], (
            f"total_requests 不该减少：第一次={first['total_requests']}，第二次={second['total_requests']}"
        )

    def test_h04_alive_connections_at_least_one(self, server_addr: ServerAddr):
        """用例 H-04：当前至少占着一条连接时，alive_connections >= 1。

        做法：自己开一条裸 socket，**保持不关**，就在这条连接上查 /metrics。
        既然这条连接此刻是活的，服务端统计的 alive_connections 至少得是 1。
        如果服务端用 requests.Session 不保证连接还在（空闲可能被回收），
        这个自持连接的做法就不会有歧义。
        """
        print("[H-04] 持有一条连接的同时查 /metrics，验证 alive_connections >= 1")
        sock = socket_connect(server_addr)
        try:
            request = build_raw_request(
                "GET", "/metrics", headers={"Host": f"{server_addr.host}:{server_addr.port}"}
            )
            sock.sendall(request)
            _headers, body = read_one_response(sock)
            metrics = parse_metrics(body.decode("utf-8", errors="replace"))
            assert "alive_connections" in metrics, f"指标里没有 alive_connections：{body[:300]!r}"
            assert metrics["alive_connections"] >= 1, (
                f"此刻本用例正占着一条活连接，alive_connections 应该 >= 1，实际是 {metrics['alive_connections']}"
            )
        finally:
            try:
                sock.close()
            except OSError:
                pass

    def test_h05_metrics_line_format(self, api):
        """用例 H-05：/metrics 每一行都是「键 值」两段，键里不含空格。

        格式看起来是小事，但监控系统是靠"按空格切两段"来读指标的。
        键里混进空格，整行就会被解析错（值跑到键里去）。
        """
        print("[H-05] 检查 /metrics 的行格式")
        resp = api.metrics_raw()
        assert resp.status_code == 200, f"期望 200，实际 {resp.status_code}"
        checked = 0
        for line_no, line in enumerate(resp.text.splitlines(), start=1):
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue  # 空行和注释行允许存在
            parts = stripped.split()
            assert len(parts) == 2, f"第 {line_no} 行不是「键 值」两段：{line!r}"
            key, value = parts
            assert " " not in key, f"第 {line_no} 行的键里混进了空格：{key!r}"
            try:
                float(value)
            except ValueError:
                pytest.fail(f"第 {line_no} 行的值不是数字：{line!r}")
            checked += 1
        assert checked >= 7, f"至少应该有 7 行指标，实际只解析出 {checked} 行：{resp.text[:300]!r}"


# ===========================================================================
# 五、响应公共头
# ===========================================================================
class TestResponseHeaders:
    """契约规定：所有响应都要带 Content-Length 和 Content-Type（用例编号 G-xx）。"""

    @pytest.mark.parametrize(
        "kind",
        ["healthz", "metrics", "shorten", "redirect", "stats", "notfound", "method_not_allowed"],
        ids=["健康检查", "指标", "创建成功", "跳转302", "统计", "404错误", "405错误"],
    )
    def test_g01_common_headers(self, api, kind, shorten, unique_url):
        """用例 G-01：7 种响应（成功 + 各种错误）都必须带 Content-Length 和 Content-Type。

        为什么要一口气测 7 种？因为最容易漏的是"错误响应"——
        很多实现在正常路径上补头补得很好，一走到 404/405 就忘了，
        客户端拿不到 Content-Length，就只能一直等连接关闭（表现为"卡住"）。
        """
        print(f"[G-01] 检查 {kind} 响应的公共头")
        if kind == "healthz":
            resp = api.healthz()
        elif kind == "metrics":
            resp = api.metrics_raw()
        elif kind == "shorten":
            resp = api.shorten(unique_url("g01"))
        elif kind == "redirect":
            resp = api.redirect(shorten(unique_url("g01")))
        elif kind == "stats":
            resp = api.stats(shorten(unique_url("g01")))
        elif kind == "notfound":
            resp = api.get("/zzzzzzzz")
        else:
            resp = api.request("DELETE", "/api/shorten")

        assert resp.headers.get("Content-Type"), f"{kind} 响应缺少 Content-Type 头；头={dict(resp.headers)!r}"
        content_length = resp.headers.get("Content-Length")
        assert content_length is not None, f"{kind} 响应缺少 Content-Length 头；头={dict(resp.headers)!r}"
        assert content_length.isdigit(), f"{kind} 的 Content-Length 应该是纯数字：{content_length!r}"
        assert int(content_length) == len(resp.content), (
            f"{kind} 的 Content-Length 和实际 body 长度对不上：声明 {content_length}，实际 {len(resp.content)}"
        )


# ===========================================================================
# 六、方法不支持  405
# ===========================================================================
class TestMethodNotAllowed:
    """方法不支持相关用例（用例编号 M-xx）。"""

    @pytest.mark.parametrize(
        "method,path",
        [
            ("DELETE", "/api/shorten"),
            ("PUT", "/api/shorten"),
            ("PATCH", "/api/shorten"),
            ("POST", "/healthz"),
            ("POST", "/metrics"),
            ("DELETE", "/healthz"),
        ],
        ids=["DELETE创建", "PUT创建", "PATCH创建", "POST健康检查", "POST指标", "DELETE健康检查"],
    )
    def test_m01_method_not_allowed(self, api, method, path):
        """用例 M-01：方法用错了 → 405 method_not_allowed，且响应必须带 Allow 头。

        Allow 头是 HTTP 的"礼貌提示"：告诉客户端"这个路径只支持这些方法"。
        少了它，调用方只能靠猜。所以契约把它写成了硬要求。
        """
        print(f"[M-01] {method} {path}（该方法不受支持）")
        resp = api.request(method, path)
        assert_error_body(resp, 405, "method_not_allowed")
        assert resp.headers.get("Allow"), (
            f"405 响应必须带 Allow 头，实际头={dict(resp.headers)!r}（客户端需要知道该用哪个方法）"
        )

    def test_m02_delete_code_not_allowed(self, api, shorten, unique_url):
        """用例 M-02：DELETE /{code} → 405，且 Allow 头里含有 GET。

        v1 不支持删除短链，所以这条路径只允许 GET。允许删除是很危险的（容易被恶意刷掉数据）。
        """
        print("[M-02] DELETE /{code} 应当被拒绝为 405，且 Allow 头里含 GET")
        code = shorten(unique_url("m02"))
        resp = api.request("DELETE", "/" + code)
        assert_error_body(resp, 405, "method_not_allowed")
        allow = resp.headers.get("Allow", "")
        assert "GET" in allow.upper(), f"Allow 头里应该含 GET，实际是 {allow!r}"

    @pytest.mark.parametrize(
        "path,expected_allow",
        [("/api/shorten", "POST"), ("/healthz", "GET"), ("/metrics", "GET")],
        ids=["创建只允许POST", "健康检查只允许GET", "指标只允许GET"],
    )
    def test_m03_allow_header_content(self, api, path, expected_allow):
        """用例 M-03：405 的 Allow 头要**说对**支持哪个方法，不能随便糊一个。

        只检查"有 Allow 头"还不够：如果 /healthz 的 Allow 写成了 POST，
        客户端照着改还是错。所以这里逐个核对内容。
        """
        print(f"[M-03] DELETE {path}，Allow 头里应当含 {expected_allow}")
        resp = api.request("DELETE", path)
        assert_error_body(resp, 405, "method_not_allowed")
        allow = resp.headers.get("Allow", "")
        assert expected_allow in allow.upper(), f"Allow 头里应该含 {expected_allow}，实际是 {allow!r}"


# ===========================================================================
# 七、各种上限  413 / 431 / 501 / 408
# ===========================================================================
class TestLimits:
    """请求大小与超时相关的用例（用例编号 L-xx）。"""

    def test_l01_body_too_large_413(self, server_addr: ServerAddr):
        """用例 L-01：请求体超过 8 KiB → 413 payload_too_large。

        为什么要限制 body 大小？因为不限制的话，别人发一个 10 GB 的请求就能把内存吃光，
        这是最经典的一类"拒绝服务"攻击。所以必须在读之前就把门关上。
        """
        big_url = "https://example.com/" + ("a" * (10 * 1024))
        body = json.dumps({"url": big_url}).encode("utf-8")
        assert len(body) > LIMIT_8K, "前置条件：body 必须真的超过 8 KiB"
        payload = build_raw_request(
            "POST",
            "/api/shorten",
            headers={
                "Host": f"{server_addr.host}:{server_addr.port}",
                "Content-Type": "application/json",
                "Connection": "close",
            },
            body=body,
        )
        print(f"[L-01] 用裸 socket 发一个 {len(body)} 字节的 body（上限 {LIMIT_8K} 字节）")
        data = raw_http(server_addr.host, server_addr.port, payload, read_timeout=5.0)
        assert_raw_error(data, 413, "payload_too_large")

    @pytest.mark.parametrize("style", ["单个超长头", "很多小头"])
    def test_l02_headers_too_large_431(self, server_addr: ServerAddr, style):
        """用例 L-02：请求行 + 请求头总长超过 8 KiB → 431 headers_too_large。

        同样是为了防"内存被请求头撑爆"。两种写法都要拦：
        一个特别长的头，和一堆很短的头——它们的总长是一样的。
        """
        if style == "单个超长头":
            headers = {"Host": f"{server_addr.host}:{server_addr.port}", "X-Big": "b" * (10 * 1024)}
        else:
            headers = {"Host": f"{server_addr.host}:{server_addr.port}"}
            for index in range(400):
                headers[f"X-Filler-{index:03d}"] = "c" * 32

        payload = build_raw_request("GET", "/healthz", headers=headers)
        total_head_len = len(payload.split(b"\r\n\r\n", 1)[0])
        print(f"[L-02] {style}：请求头总长约 {total_head_len} 字节（上限 {LIMIT_8K}）")
        data = raw_http(server_addr.host, server_addr.port, payload, read_timeout=5.0)
        assert_raw_error(data, 431, "headers_too_large")

    @pytest.mark.parametrize("chunk_body", [b'{"url":"https://example.com/chunked"}', b"x" * 256], ids=["正常块", "较长块"])
    def test_l03_chunked_not_supported_501(self, server_addr: ServerAddr, chunk_body):
        """用例 L-03：带 Transfer-Encoding: chunked 的请求 → 501 unsupported_transfer_encoding。

        chunked 的意思是"我边算边发，事先不知道总长度"，它比 Content-Length 复杂得多。
        v1 明确不支持，所以必须**明确拒绝**（501），而不是装傻当成没 body 处理——
        那样会把用户的数据悄悄丢掉，比报错还糟糕。
        """
        chunked_body = b"%x\r\n" % len(chunk_body) + chunk_body + b"\r\n0\r\n\r\n"
        payload = (
            f"POST /api/shorten HTTP/1.1\r\n"
            f"Host: {server_addr.host}:{server_addr.port}\r\n"
            f"Content-Type: application/json\r\n"
            f"Transfer-Encoding: chunked\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        ).encode("latin-1") + chunked_body
        print("[L-03] 用裸 socket 发 chunked 请求体")
        data = raw_http(server_addr.host, server_addr.port, payload, read_timeout=5.0)
        assert_raw_error(data, 501, "unsupported_transfer_encoding")

    @pytest.mark.slow
    def test_l04_read_timeout_408(self, server_addr: ServerAddr, run_slow):
        """用例 L-04：只发半行请求就不发了，等 6 秒 → 408 request_timeout。

        这条测的是"读超时"：服务端不能在半个请求上无限等下去，
        否则攻击者只要连上一堆连接、每个都发一半，就能把连接数耗光（慢速攻击）。
        默认跳过是因为它要实打实等 5 秒以上，跑全量时太拖时间；用 --run-slow 打开。
        """
        if not run_slow:
            pytest.skip("慢用例默认跳过：加 --run-slow 才会真的跑（要等 6 秒以上）")

        print("[L-04] 只发半行请求行，然后干等 6 秒，期望收到 408")
        # 故意半截的请求行：服务端在 5 秒内收不到完整请求，就应该回 408 并关连接
        payload = b"GET /heal"
        data = raw_http(server_addr.host, server_addr.port, payload, read_timeout=9.0)
        assert data, "等了 6 秒也没收到任何响应（读超时没生效？）"
        assert_raw_error(data, 408, "request_timeout")


# ===========================================================================
# 八、keep-alive 连接复用
# ===========================================================================
class TestKeepAlive:
    """长连接相关用例（用例编号 K-xx）。"""

    def test_k01_session_reuses_connection(self, base_url, api, server_addr: ServerAddr, http_timeout):
        """用例 K-01：用 requests.Session 连发 3 个请求，并用 http.client 验证"走的是同一个本地端口"。

        为什么盯着"本地端口"？每建立一条新的 TCP 连接，操作系统就会分配一个新的本地端口号。
        三次请求如果端口号完全一样，就证明它们**复用了同一条连接**，
        也就是 keep-alive 真的生效了（否则每次都要重新握手，性能差一大截）。
        """
        print("[K-01] 同一个 Session 连发 3 个请求，检查连接是否被复用")
        with allure_step("同一个 Session 连发 3 次 /healthz"):
            for index in range(3):
                resp = api.get("/healthz")
                assert resp.status_code == 200, f"第 {index + 1} 次请求失败：{resp.status_code}"

        # 再用 http.client 显式看一眼本地端口：它比 requests 暴露得更多
        conn = http.client.HTTPConnection(server_addr.host, server_addr.port, timeout=http_timeout)
        local_ports = set()
        try:
            for index in range(3):
                conn.request("GET", "/healthz")
                resp = conn.getresponse()
                body = resp.read()
                assert resp.status == 200, f"http.client 第 {index + 1} 次请求失败：{resp.status}"
                assert body == b"ok", f"body 应该是 ok，实际 {body!r}"
                local_ports.add(conn.sock.getsockname()[1])
        finally:
            conn.close()

        assert len(local_ports) == 1, (
            f"3 次请求应该复用同一条 TCP 连接（本地端口相同），实际出现了 {len(local_ports)} 个端口：{local_ports}"
        )
        print(f"[K-01] 三次请求的本地端口都是 {local_ports.pop()}，连接确实被复用了")

    def test_k02_raw_socket_keep_alive_two_requests(self, server_addr: ServerAddr):
        """用例 K-02：在一条裸 socket 上连发 2 个请求，两个都能正常拿到响应。

        这条是 K-01 的"低层版本"：不带任何库的帮忙，直接在同一根网线上问两次。
        如果服务端处理完一个请求就关连接，第二个请求会失败（连接已断）。
        """
        print("[K-02] 一条裸 socket 上连发 2 个请求")
        sock = socket_connect(server_addr)
        try:
            local_port = sock.getsockname()[1]
            for index in range(2):
                request = build_raw_request(
                    "GET", "/healthz", headers={"Host": f"{server_addr.host}:{server_addr.port}"}
                )
                sock.sendall(request)
                headers, body = read_one_response(sock)
                assert body == b"ok", f"第 {index + 1} 个请求的 body 应该是 ok，实际 {body!r}"
            assert sock.getsockname()[1] == local_port, "同一个 socket 的本地端口不该变"
        finally:
            sock.close()

    def test_k03_connection_close_is_honored(self, server_addr: ServerAddr):
        """用例 K-03：请求里带 Connection: close → 服务端回完响应就关连接。

        这是 HTTP 基本礼仪的另一半：客户端说了"说完就挂"，服务端就不能赖着不走。
        验证方式很直接：收完响应后继续读，如果读到 b""（对端关闭），说明连接真的断了。
        """
        print("[K-03] 带 Connection: close 的请求，服务端回完应当关闭连接")
        payload = build_raw_request(
            "GET",
            "/healthz",
            headers={"Host": f"{server_addr.host}:{server_addr.port}", "Connection": "close"},
        )
        data, closed = raw_http_until_close(server_addr.host, server_addr.port, payload, read_timeout=5.0)
        status, _headers, body = parse_http_response(data)
        assert status == 200, f"期望 200，实际 {status}；响应={data[:200]!r}"
        assert body.strip() == b"ok", f"body 应该是 ok，实际 {body!r}"
        assert closed, "服务端响应后应当主动关闭连接（读 socket 应该得到 b''），但它没有关"


# ===========================================================================
# 九、并发
# ===========================================================================
class TestConcurrency:
    """并发相关用例（用例编号 P-xx）。

    并发用例的通用约定：每个线程**必须**用自己的 requests.Session，
    因为 Session 内部维护连接池等共享状态，多线程共用一个会互相踩到，出现莫名其妙的失败。
    """

    def test_p01_concurrent_unique_codes(self, base_url, http_timeout, unique_url):
        """用例 P-01：并发创建 100 条**不同**的 url → 100 个短码全部唯一，且一次都没失败。

        这是"短码全局唯一"这条契约的压力版验证。
        如果生成短码时用了"先查再写"的非原子操作，并发下两个线程可能拿到同一个码，
        那样两个不同的网址就会互相覆盖（用户点 A 链跳到 B 页面）。
        """
        urls = [unique_url(f"p01-{index}") for index in range(100)]
        print(f"[P-01] 并发创建 {len(urls)} 条不同的短链")
        results: List[Tuple[int, dict]] = []
        errors: List[str] = []

        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            futures = [pool.submit(post_shorten_in_thread, base_url, http_timeout, url) for url in urls]
            for future in concurrent.futures.as_completed(futures):
                try:
                    results.append(future.result())
                except Exception as exc:  # 线程里抛出的异常要收集起来，最后一起报，方便定位
                    errors.append(repr(exc))

        assert not errors, f"并发创建过程中有 {len(errors)} 次失败：{errors[:5]}"
        statuses = [status for status, _body in results]
        assert statuses == [201] * 100, f"全部请求都该返回 201，实际状态码分布：{sorted(set(statuses))}"

        codes = [body["code"] for _status, body in results]
        assert len(codes) == 100, f"应该拿到 100 个短码，实际 {len(codes)} 个"
        assert len(set(codes)) == 100, (
            f"100 个不同的 url 必须对应 100 个互不相同的短码，实际只有 {len(set(codes))} 个唯一值"
        )

    def test_p02_concurrent_same_url(self, base_url, http_timeout, unique_url):
        """用例 P-02：并发创建 20 条**相同**的 url → 全部返回同一个短码。

        这是幂等性在并发下的版本。如果两个线程同时创建同一个网址、
        都发现"还不存在"然后各写一条记录，就会产生两个短码——
        数据里出现重复，之后的统计也会分裂成两份。
        """
        url = unique_url("p02-shared")
        print(f"[P-02] 并发创建 20 次同一个 url：{url}")
        results: List[Tuple[int, dict]] = []
        errors: List[str] = []

        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as pool:
            futures = [pool.submit(post_shorten_in_thread, base_url, http_timeout, url) for _ in range(20)]
            for future in concurrent.futures.as_completed(futures):
                try:
                    results.append(future.result())
                except Exception as exc:
                    errors.append(repr(exc))

        assert not errors, f"并发创建过程中有 {len(errors)} 次失败：{errors[:5]}"
        statuses = sorted({status for status, _body in results})
        assert statuses == [201], f"全部请求都该返回 201，实际状态码集合：{statuses}"
        codes = {body["code"] for _status, body in results}
        assert len(codes) == 1, f"同一个 url 并发创建，必须只得到一个短码，实际得到 {len(codes)} 个：{sorted(codes)}"

    def test_p03_concurrent_redirect_hits(self, api, base_url, http_timeout, shorten, unique_url):
        """用例 P-03：并发跳转同一个短码 100 次 → hits 精确等于 100。

        这测的是计数的原子性。如果服务端写的是 `hits = hits + 1`（先读后写，中间没加锁），
        两个线程会读到同一个旧值，各加一次，最后只涨了 1 —— 统计就少算了。
        这正是"压测发现的问题单靠单线程测不出来"的典型例子。
        """
        print("[P-03] 并发跳转同一个短码 100 次，hits 应当精确涨 100")
        code = shorten(unique_url("p03"))
        before = get_hits(api, code)
        statuses: List[int] = []
        errors: List[str] = []

        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            futures = [pool.submit(get_redirect_in_thread, base_url, http_timeout, code) for _ in range(100)]
            for future in concurrent.futures.as_completed(futures):
                try:
                    statuses.append(future.result())
                except Exception as exc:
                    errors.append(repr(exc))

        assert not errors, f"并发跳转过程中有 {len(errors)} 次失败：{errors[:5]}"
        assert set(statuses) == {302}, f"100 次跳转都该返回 302，实际状态码集合：{sorted(set(statuses))}"

        after = get_hits(api, code)
        assert after - before == 100, (
            f"并发跳转 100 次，hits 应该正好涨 100；实际从 {before} 涨到 {after}（只涨了 {after - before}）。"
            f"可能是计数竞态：多个线程同时读到同一个旧值，各自 +1 后互相覆盖。"
        )


# ===========================================================================
# 十、其它补充分支（把边角情况也钉住）
# ===========================================================================
class TestMisc:
    """补充用例（用例编号 X-xx）。"""

    def test_x01_same_code_content_consistent(self, api, shorten, unique_url):
        """用例 X-01：同一个 url 创建的短码，跳转和统计里的 url 必须一致。

        三处地方（创建响应、Location 头、统计接口）存的是同一个网址，
        任何一处不一致都说明数据是"各写各的"，不是单一数据源。
        """
        url = unique_url("x01")
        print(f"[X-01] 交叉核对创建、跳转、统计三个接口里的 url 是否一致；url={url}")
        code = shorten(url)

        redirect = api.redirect(code)
        assert redirect.status_code == 302, f"期望 302，实际 {redirect.status_code}"
        assert redirect.headers.get("Location") == url, "跳转的 Location 和创建时的 url 不一致"

        stats = api.stats(code)
        assert stats.status_code == 200, f"期望 200，实际 {stats.status_code}"
        assert stats.json()["url"] == url, "统计接口里的 url 和创建时的 url 不一致"

    @pytest.mark.parametrize("path", ["/api/shorten/", "/api/stats/", "/unknown", "/api/unknown"])
    def test_x02_unknown_paths(self, api, path):
        """用例 X-02：访问没定义的路径 → 404 not_found（统一错误格式）。

        注意 /api/shorten/ 和 /api/stats/ 这种"结尾多个斜杠"的写法：
        契约里每个路径都是写死的，所以它们属于"不存在"。
        这里只要求错误体是统一格式，不强制一定 404——有些实现会给 400，也说得过去。
        """
        print(f"[X-02] 访问未定义的路径 {path}")
        resp = api.get(path)
        assert resp.status_code in (400, 404, 405), f"未定义路径不该是 {resp.status_code}；响应={resp.text[:200]!r}"
        if resp.status_code in (400, 404):
            body = assert_error_body(resp, resp.status_code)
            assert body["error"] in ("not_found", "invalid_code", "invalid_url"), f"error 取值超出合理范围：{body!r}"

    def test_x03_shorten_updates_metrics(self, api, unique_url):
        """用例 X-03：创建短链会让 /metrics 里的 shorten_total 增加至少 1。

        业务指标（创建了多少条、跳转了多少次）是运维看板的核心数据。
        如果它不涨，看板就是"死"的，出问题时完全看不出服务到底在不在干活。
        """
        print("[X-03] 创建一条短链，观察 /metrics 的 shorten_total 是否增加")
        before = api.metrics()
        api.shorten(unique_url("x03"))
        after = api.metrics()
        assert "shorten_total" in before and "shorten_total" in after, f"指标缺字段：{before} / {after}"
        assert after["shorten_total"] >= before["shorten_total"] + 1, (
            f"创建了 1 条短链，shorten_total 至少该涨 1：创建前={before['shorten_total']}，创建后={after['shorten_total']}"
        )

    def test_x04_redirect_updates_metrics(self, api, shorten, unique_url):
        """用例 X-04：跳转一次会让 /metrics 里的 redirect_total 增加至少 1。"""
        print("[X-04] 跳转一次，观察 /metrics 的 redirect_total 是否至少涨 1")
        code = shorten(unique_url("x04"))
        before = api.metrics()
        assert api.redirect(code).status_code == 302, "跳转应当返回 302"
        after = api.metrics()
        assert "redirect_total" in before and "redirect_total" in after, f"指标缺字段：{before} / {after}"
        assert after["redirect_total"] >= before["redirect_total"] + 1, (
            f"跳转 1 次，redirect_total 至少该涨 1：跳转前={before['redirect_total']}，跳转后={after['redirect_total']}"
        )

    def test_x05_uptime_is_positive(self, api):
        """用例 X-05：uptime_seconds 是正数（服务至少活了 0 秒以上，不该是负数）。

        这条看着很傻，但它能一眼抓出"时间戳算反了""用了未初始化的变量"这类低级错误。
        """
        print("[X-05] uptime_seconds 应该是非负数（服务至少活了 0 秒）")
        metrics = api.metrics()
        assert "uptime_seconds" in metrics, f"指标里没有 uptime_seconds：{metrics!r}"
        assert metrics["uptime_seconds"] >= 0, f"uptime_seconds 不该是负数：{metrics['uptime_seconds']}"
