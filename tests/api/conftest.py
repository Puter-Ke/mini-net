# -*- coding: utf-8 -*-
"""
mini-net 接口自动化测试 —— 公共配置（conftest.py）

这个文件里放的是"所有用例都要用到的公共零件"，pytest 会自动加载它：

1) 命令行开关（--base-url / --http-timeout / --run-slow）
   用来切换被测服务的地址、统一调整超时时间。
2) fixture（夹具）
   可以理解成"自动备料"：用例只要把名字写进函数参数里，pytest 就会把这份料递过来。
3) 原始 socket 小工具
   用来发 requests 发不出来的"畸形请求"（超大 body、超大请求头、chunked 等）。
   为什么需要它？因为 requests 这个库很"讲文明"：它会自动纠正 URL、自动补 Content-Length，
   我们想故意发"坏请求"就没法用它了，只能自己拿 socket 把字节直接塞进连接里。

关于 allure：它是"测试报告美化插件"，属于**可选**依赖。
本机没装也能正常跑测试，只是没有漂亮的报告，所以下面用 try/except 包了一层。
"""

from __future__ import annotations

import contextlib
import json
import os
import re
import socket
import uuid
from typing import Dict, NamedTuple, Optional, Tuple
from urllib.parse import urlsplit

import pytest
import requests

try:
    import allure  # 可选的报告插件；没装就是 None
except ImportError:  # pragma: no cover - 取决于本机装没装
    allure = None


# 服务默认地址：命令行 > 环境变量 MININET_BASE_URL > 这个默认值
DEFAULT_BASE_URL = "http://127.0.0.1:8080"

# 短码规则：1~8 位，只允许 0-9 a-z A-Z，且大小写敏感
# 写成常量放在这里，是为了让"规则"只有一个出处，改的时候只改一处。
CODE_RE = re.compile(r"^[0-9a-zA-Z]{1,8}$")


# ---------------------------------------------------------------------------
# 小工具：条件式 allure 步骤
# ---------------------------------------------------------------------------
@contextlib.contextmanager
def allure_step(title: str):
    """给测试报告加一个中文步骤名。

    装了 allure 时，这段代码会出现在报告的步骤树里；
    没装时就用 nullcontext()（一个"什么都不做"的空壳），保证代码照样能跑。
    """
    if allure is not None:
        with allure.step(title):
            yield
    else:
        with contextlib.nullcontext():
            yield


# ---------------------------------------------------------------------------
# 命令行参数
# ---------------------------------------------------------------------------
def pytest_addoption(parser):
    """注册本套测试专用的命令行开关。"""
    group = parser.getgroup("mini-net", "mini-net 接口测试选项")
    group.addoption(
        "--base-url",
        action="store",
        dest="base_url",
        default=os.environ.get("MININET_BASE_URL", DEFAULT_BASE_URL),
        help="被测服务地址；不传就读环境变量 MININET_BASE_URL，再默认 http://127.0.0.1:8080",
    )
    group.addoption(
        "--http-timeout",
        action="store",
        dest="http_timeout",
        type=float,
        default=10.0,
        help="每个 HTTP 请求等待响应的秒数，默认 10 秒",
    )
    group.addoption(
        "--run-slow",
        action="store_true",
        dest="run_slow",
        default=False,
        help="打开后才跑 -- 慢用例（例如要等 5 秒以上才会返回 408 的那条），默认跳过",
    )


def pytest_configure(config):
    """注册自定义 marker，顺便让 `pytest --markers` 能看到中文说明。"""
    config.addinivalue_line("markers", "slow: 很慢的用例（要等 5 秒以上），默认跳过，加 --run-slow 才会跑")
    config.addinivalue_line("markers", "smoke: 冒烟用例，最核心的几条，快速验证服务是否活着")
    config.addinivalue_line("markers", "api: 所有 HTTP 接口用例（本文件下几乎全是）")


# ---------------------------------------------------------------------------
# 地址解析
# ---------------------------------------------------------------------------
class ServerAddr(NamedTuple):
    """服务地址被拆开后的样子：主机、端口、协议。"""

    host: str
    port: int
    scheme: str


def split_base_url(base_url: str) -> ServerAddr:
    """把 http://127.0.0.1:8080 拆成 ("127.0.0.1", 8080, "http")。

    拆分是为了给"裸 socket"用：socket 只认 IP + 端口，不认 URL。
    """
    parts = urlsplit(base_url)
    scheme = (parts.scheme or "http").lower()
    host = parts.hostname or "127.0.0.1"
    port = parts.port or (443 if scheme == "https" else 80)
    return ServerAddr(host=host, port=port, scheme=scheme)


# ---------------------------------------------------------------------------
# 原始 socket 工具（本文件里最"硬核"的部分）
# ---------------------------------------------------------------------------
def _raw_http_impl(
    host: str,
    port: int,
    payload: bytes,
    read_timeout: float = 3.0,
    expect_bytes: Optional[int] = None,
    connect_timeout: float = 5.0,
) -> Tuple[bytes, bool]:
    """真正干活的函数：发原始字节、尽力收响应，返回 (收到的字节, 对端是否关闭了连接)。

    这里要容忍两种"看起来很吓人、其实很正常"的情况：
    - 服务端读完请求就回响应并关连接：我们继续读会读到空，这是正常的结束信号。
    - 我们还在写数据时连接被服务端 RST 掉：发送会抛 BrokenPipeError/ConnectionResetError，
      这通常说明服务端"提前拒绝"了（比如请求头太大直接 431），不是测试失败，忽略即可。
    """
    received = bytearray()
    closed = False

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(connect_timeout)
    try:
        sock.connect((host, port))
    except OSError as exc:
        sock.close()
        raise AssertionError(
            f"连接 {host}:{port} 失败：{exc}（服务是不是没启动？或者 --base-url 写错了？）"
        ) from exc

    sock.settimeout(read_timeout)
    try:
        try:
            sock.sendall(payload)
        except OSError:
            # 见上面 docstring：发送过程中被 RST 属于预期内，忽略，继续尝试读响应
            pass

        while True:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                # 读超时：说明服务端既不回数据也没关连接，把已经收到的返回即可
                break
            except OSError:
                closed = True
                break
            if not chunk:
                closed = True
                break
            received.extend(chunk)
            if expect_bytes is not None and len(received) >= expect_bytes:
                break
    finally:
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        sock.close()

    return bytes(received), closed


def raw_http(
    host: str,
    port: int,
    payload: bytes,
    read_timeout: float = 3.0,
    expect_bytes: Optional[int] = None,
    connect_timeout: float = 5.0,
) -> bytes:
    """把一个请求的原始字节直接发给服务端，返回收到的原始响应字节。

    用途：发那些 requests 不肯帮我们发的畸形请求。
    - payload：一整段 HTTP 报文（请求行 + 头 + 空行 + body），用 bytes 拼好。
    - read_timeout：每次 recv 等多久；超时了就带着已收到的数据返回，不会卡死用例。
    - expect_bytes：收够这么多字节就停（不知道长度时传 None，靠超时或对端关连接结束）。
    """
    data, _closed = _raw_http_impl(
        host, port, payload, read_timeout=read_timeout, expect_bytes=expect_bytes, connect_timeout=connect_timeout
    )
    return data


def raw_http_until_close(host: str, port: int, payload: bytes, read_timeout: float = 3.0) -> Tuple[bytes, bool]:
    """和 raw_http 一样，但额外告诉你"连接是不是被对端关了"。

    返回 (收到的字节, 是否关闭)。用来验证 `Connection: close` 之后服务端真的挂了电话。
    """
    return _raw_http_impl(host, port, payload, read_timeout=read_timeout)


def build_raw_request(
    method: str,
    path: str,
    headers: Optional[Dict[str, str]] = None,
    body: bytes = b"",
    host_header: str = "127.0.0.1",
) -> bytes:
    """手工拼一个完整的 HTTP 请求报文（bytes）。

    自己拼的好处是：想写多长写多长、想写多怪写多怪，requests 管不着。
    """
    lines = [f"{method} {path} HTTP/1.1", f"Host: {host_header}"]
    for key, value in (headers or {}).items():
        lines.append(f"{key}: {value}")
    lines.append(f"Content-Length: {len(body)}")
    head = ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1")
    return head + body


def parse_http_response(data: bytes) -> Tuple[int, Dict[str, str], bytes]:
    """把原始响应字节拆成 (状态码, 头字典, body)。

    只做最基本的解析：因为我们关心的就是状态码和 JSON body，
    没必要为了测试再写一个完整的 HTTP 解析器。
    """
    if b"\r\n\r\n" not in data:
        pytest.fail(f"响应不完整，连头部的空行都没有收到；实际收到 {data[:300]!r}")

    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status_line = lines[0].decode("latin-1")
    pieces = status_line.split(" ", 2)
    if len(pieces) < 2 or not pieces[1].isdigit():
        pytest.fail(f"响应状态行看不懂：{status_line!r}")

    status = int(pieces[1])
    headers: Dict[str, str] = {}
    for line in lines[1:]:
        if b":" not in line:
            continue
        key, _, value = line.partition(b":")
        headers[key.decode("latin-1").strip().lower()] = value.decode("latin-1").strip()
    return status, headers, body


def read_one_response(sock: socket.socket) -> Tuple[Dict[str, str], bytes]:
    """在**已经建立好的 socket** 上读一条 HTTP 响应（按 Content-Length 判断读完没有）。

    为什么要保留 socket 不关？因为有的用例要验证"当前这条连接还活着"，
    比如 /metrics 里的 alive_connections 至少是 1。
    返回 (头字典, body)。
    """
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            pytest.fail(f"读响应头时连接就断了，已收到 {buf[:300]!r}")
        buf += chunk

    head, _, body = buf.partition(b"\r\n\r\n")
    headers: Dict[str, str] = {}
    for line in head.split(b"\r\n")[1:]:
        if b":" not in line:
            continue
        key, _, value = line.partition(b":")
        headers[key.decode("latin-1").strip().lower()] = value.decode("latin-1").strip()

    length = int(headers.get("content-length", "0") or 0)
    while len(body) < length:
        chunk = sock.recv(4096)
        if not chunk:
            break
        body += chunk
    return headers, body


# ---------------------------------------------------------------------------
# 断言工具
# ---------------------------------------------------------------------------
def assert_error_body(resp: requests.Response, expected_status: int, expected_error: Optional[str] = None) -> dict:
    """断言"错误响应"符合契约：状态码对得上 + 统一错误格式 + error 字段取值对。

    统一错误格式长这样：{"error":"not_found","detail":"人类看得懂的说明"}
    error 是给程序看的（机器可读），detail 是给人看的。
    """
    assert resp.status_code == expected_status, (
        f"状态码不对：期望 {expected_status}，实际 {resp.status_code}；"
        f"url={resp.request.method} {resp.request.url}；响应体={resp.text[:300]!r}"
    )

    content_type = resp.headers.get("Content-Type", "")
    assert "json" in content_type.lower(), f"错误响应的 Content-Type 应该是 JSON，实际是 {content_type!r}"

    try:
        body = resp.json()
    except ValueError:
        pytest.fail(f"错误响应的 body 不是合法 JSON：{resp.text[:300]!r}")

    assert isinstance(body, dict), f"错误响应的 body 应该是 JSON 对象，实际是 {type(body).__name__}：{body!r}"
    assert "error" in body, f"统一错误格式缺少 error 字段：{body!r}"
    assert isinstance(body["error"], str) and body["error"], f"error 应该是非空字符串：{body!r}"
    assert "detail" in body, f"统一错误格式缺少 detail 字段：{body!r}"
    assert isinstance(body["detail"], str), f"detail 应该是字符串：{body!r}"

    if expected_error is not None:
        assert body["error"] == expected_error, (
            f"error 取值不对：期望 {expected_error!r}，实际 {body['error']!r}；完整响应={body!r}"
        )
    return body


def assert_raw_error(data: bytes, expected_status: int, expected_error: Optional[str] = None) -> dict:
    """和 assert_error_body 一样，只不过吃的是"裸 socket 收到的原始字节"。"""
    status, _headers, body_bytes = parse_http_response(data)
    assert status == expected_status, (
        f"状态码不对：期望 {expected_status}，实际 {status}；响应={data[:300]!r}"
    )
    try:
        body = json.loads(body_bytes.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        pytest.fail(f"错误响应的 body 不是合法 JSON：{body_bytes[:300]!r}")

    assert isinstance(body, dict), f"错误响应的 body 应该是 JSON 对象：{body!r}"
    assert isinstance(body.get("error"), str) and body["error"], f"error 应该是非空字符串：{body!r}"
    assert "detail" in body, f"统一错误格式缺少 detail 字段：{body!r}"
    if expected_error is not None:
        assert body["error"] == expected_error, (
            f"error 取值不对：期望 {expected_error!r}，实际 {body['error']!r}；完整响应={body!r}"
        )
    return body


def parse_metrics(text: str) -> Dict[str, float]:
    """把 /metrics 的正文解析成 {指标名: 数值}。

    容忍空行和 # 注释行；解析不出来的行跳过（格式是否严格由专门的用例去挑刺）。
    """
    result: Dict[str, float] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 2:
            continue
        try:
            result[parts[0]] = float(parts[1])
        except ValueError:
            continue
    return result


# ---------------------------------------------------------------------------
# 请求封装
# ---------------------------------------------------------------------------
class ApiClient:
    """对 requests.Session 的一层薄封装。

    为什么用 Session 而不是每次都 requests.get？
    因为 Session 会把 TCP 连接留着复用（keep-alive），就像打电话不挂断，
    说完一件事接着说下一件。这正好也是我们要验证的服务端能力之一。
    """

    def __init__(self, base_url: str, timeout: float):
        self.base_url = base_url
        self.timeout = timeout
        self.server = split_base_url(base_url)
        self.session = requests.Session()

    # ---- 基础能力 ----
    def url(self, path: str) -> str:
        """把 /api/shorten 这种路径拼成完整网址。"""
        if not path.startswith("/"):
            path = "/" + path
        return self.base_url + path

    def request(self, method: str, path: str, **kwargs) -> requests.Response:
        """所有请求的统一入口：自动补超时、自动关掉重定向跟随。"""
        kwargs.setdefault("timeout", self.timeout)
        # 关掉自动跟随重定向，否则 302 会被 requests 悄悄跟到目标网址，
        # 我们就看不到 Location 头了（而跳转测试恰恰要断言这个头）。
        kwargs.setdefault("allow_redirects", False)
        return self.session.request(method, self.url(path), **kwargs)

    def get(self, path: str, **kwargs) -> requests.Response:
        return self.request("GET", path, **kwargs)

    def post_json(self, path: str, payload=None, raw_body: Optional[bytes] = None, headers=None, **kwargs):
        """发一个 JSON 请求。

        raw_body 用来发"故意的坏 JSON"（比如 {not json），绕过 json.dumps。
        """
        hdrs = {"Content-Type": "application/json"}
        if headers:
            hdrs.update(headers)
        if raw_body is not None:
            return self.request("POST", path, data=raw_body, headers=hdrs, **kwargs)
        return self.request("POST", path, json=payload, headers=hdrs, **kwargs)

    # ---- 业务便捷方法 ----
    def shorten(self, url: str) -> requests.Response:
        return self.post_json("/api/shorten", {"url": url})

    def redirect(self, code: str) -> requests.Response:
        return self.get("/" + code)

    def stats(self, code: str) -> requests.Response:
        return self.get("/api/stats/" + code)

    def healthz(self) -> requests.Response:
        return self.get("/healthz")

    def metrics_raw(self) -> requests.Response:
        return self.get("/metrics")

    def metrics(self) -> Dict[str, float]:
        resp = self.metrics_raw()
        assert resp.status_code == 200, f"/metrics 应该返回 200，实际 {resp.status_code}；响应={resp.text[:300]!r}"
        return parse_metrics(resp.text)

    def close(self) -> None:
        self.session.close()


# ---------------------------------------------------------------------------
# fixture
# ---------------------------------------------------------------------------
@pytest.fixture(scope="session")
def base_url(request) -> str:
    """被测服务地址，去掉结尾多余的 '/'（拼路径时就不会出现双斜杠）。"""
    raw = request.config.getoption("--base-url")
    return raw.rstrip("/")


@pytest.fixture(scope="session")
def http_timeout(request) -> float:
    """每个 HTTP 请求等响应的秒数，来自 --http-timeout。"""
    return float(request.config.getoption("--http-timeout"))


@pytest.fixture(scope="session")
def run_slow(request) -> bool:
    """慢用例开关，来自 --run-slow。默认 False。"""
    return bool(request.config.getoption("--run-slow"))


@pytest.fixture(scope="session")
def server_addr(base_url: str) -> ServerAddr:
    """服务的 主机/端口/协议，供裸 socket 用例使用。"""
    return split_base_url(base_url)


@pytest.fixture
def api(base_url: str, http_timeout: float) -> ApiClient:
    """每个用例一个新的会话式 HTTP 客户端（用例之间互不干扰）。"""
    client = ApiClient(base_url, http_timeout)
    try:
        yield client
    finally:
        client.close()


@pytest.fixture
def shorten(api: ApiClient):
    """便捷函数：shorten(url) -> 短码。失败直接让用例挂掉，并说清为什么。"""

    def _shorten(url: str) -> str:
        resp = api.shorten(url)
        if resp.status_code != 201:
            pytest.fail(
                f"准备测试数据失败：创建短链返回 {resp.status_code}（期望 201）；"
                f"url={url!r}；响应体={resp.text[:300]!r}"
            )
        body = resp.json()
        if "code" not in body:
            pytest.fail(f"创建短链的响应里没有 code 字段：{body!r}")
        return body["code"]

    return _shorten


@pytest.fixture
def unique_url():
    """工厂函数：每次调用都产出一个**不重样**的合法 URL。

    为什么需要它？因为服务端对同一个 url 会返回同一个短码。
    如果两个用例用了同一个 url，hits 之类的计数就会互相污染，
    排查起来特别痛苦。所以每条用例都用带随机串的新 url。
    """

    def _make(hint: str = "case") -> str:
        token = uuid.uuid4().hex
        return f"https://example.com/{hint}/{token}?t={token[:8]}"

    return _make
