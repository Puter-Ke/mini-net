# mini-net HTTP 接口规格（v1，M5 实现）

> 这份文件是**接口契约**：服务端实现（src/）和自动化测试（tests/api/）都以它为准。
> 任何一方改了行为，必须先改这份文件。

## 通用约定
- 协议：HTTP/1.1，支持 `Connection: keep-alive`
- 默认端口：8080
- 请求体上限：8 KiB（超过返回 413）
- 请求行/头总长度上限：8 KiB（超过返回 431）
- 读超时：5 秒没有收到完整请求 → 返回 408 并关闭连接
- 所有响应都带 `Content-Length` 和 `Content-Type`
- 短码字符集：`[0-9a-zA-Z]{1,8}`，大小写敏感

## 端点

### 1. 创建短链
```
POST /api/shorten
Content-Type: application/json

{"url": "https://example.com/very/long/path?x=1"}

→ 201 Created
{"code":"aB3cD","short":"http://<host>/aB3cD","url":"https://example.com/very/long/path?x=1"}

→ 400 Bad Request   （url 缺失 / 不是字符串 / 长度 > 2048 / scheme 不是 http 或 https）
{"error":"invalid_url","detail":"..."}
```
- 若同一 url 已存在，返回**同一个短码**且状态码仍为 201
- 短码必须全局唯一（并发创建也不能撞）

### 2. 跳转
```
GET /{code}
→ 302 Found
Location: https://example.com/very/long/path?x=1
Cache-Control: no-store

→ 404 Not Found    （短码不存在）
→ 400 Bad Request  （短码含非法字符）
```
每次成功跳转让该短码的 `hits` 加 1。

### 3. 统计
```
GET /api/stats/{code}
→ 200 OK
{"code":"aB3cD","url":"https://...","hits":12,"created_at":1757800000}

→ 404 Not Found
```

### 4. 健康检查
```
GET /healthz
→ 200 OK   body: ok        Content-Type: text/plain
```

### 5. 服务指标
```
GET /metrics
→ 200 OK   Content-Type: text/plain
uptime_seconds 123
alive_connections 3
total_connections 1024
total_requests 5000
total_bytes_in 320000
shorten_total 120
redirect_total 4800
```

## 错误响应统一格式
```json
{"error":"<机器可读标识>","detail":"<人类可读说明>"}
```
| 状态码 | error | 触发条件 |
|---|---|---|
| 400 | invalid_url / invalid_code | 参数非法 |
| 404 | not_found | 短码或路径不存在 |
| 405 | method_not_allowed | 方法不支持（附 `Allow` 头） |
| 408 | request_timeout | 5 秒内未收到完整请求 |
| 413 | payload_too_large | body > 8 KiB |
| 431 | headers_too_large | 请求头 > 8 KiB |
| 501 | unsupported_transfer_encoding | 收到 chunked 请求体（v1 不支持） |

## 性能目标（M6 验证）
| 场景 | 目标 |
|---|---|
| 短链创建 | ≥ 15,000 QPS（4 线程，连接复用） |
| 短链跳转 | ≥ 20,000 QPS |
| P99 延迟 | ≤ 5 ms（1000 并发连接、4 线程） |
| 内存 | 10 万条短链 + 1000 连接下 RSS < 200 MB |
