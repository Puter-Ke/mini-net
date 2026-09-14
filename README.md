# mini-net

[![CI](https://github.com/Puter-Ke/mini-net/actions/workflows/ci.yml/badge.svg)](https://github.com/Puter-Ke/mini-net/actions/workflows/ci.yml)

**从零手写的 C++17 高并发网络服务**：不依赖任何第三方网络/HTTP 框架，
自己实现 Reactor + epoll(ET) + one-loop-per-thread + 环形缓冲 + 定长对象池，
并在其上实现 HTTP/1.1 解析与短链服务（创建 / 302 跳转 / 统计 / metrics）。

> 面向的岗位：华为等大厂的 C++ 后端 / 软件开发 / 测试开发。
> 仓库里同时保留了完整的**踩坑记录**与**面试问答**（docs/面试准备.md）。

## 已完成（都有真实测试数据）

| 模块 | 内容 | 验证方式 |
|---|---|---|
| M1 | Reactor + epoll(ET) + 非阻塞 fd + EINTR/SIGPIPE 处理 | 1000 并发连接、fd 零泄漏、ASAN 干净 |
| M2 | 周期定时器 + 日志 + 空闲连接自动清理 | 集成测试 + 端到端日志证据 |
| M3 | one loop per thread + IO 线程池 + eventfd 跨线程唤醒 | 多线程回显测试、跨线程任务延迟 < 200ms 断言 |
| M4 | 环形缓冲（readv + 空间复用）+ 定长对象池 | 单测覆盖扩容/复用/半包；对象池实测结论 |
| M5 | HTTP/1.1 增量解析 + 短链业务 + 路由 | 38 个 gtest 用例 + 冒烟全链路 |
| M6 | 自写 C++ 压测客户端（非阻塞 connect + epoll，1573 行） | bench/echo_bench，结果见 docs/RESULTS.md |
| M7 | pytest 接口测试（90 用例）+ Locust + 混沌测试（5 场景） | CI 中执行，全部通过 |
| M8 | 架构文档、双语 README、面试准备 | 本文件 + docs/ |

## 快速开始（Linux / WSL / Codespaces）

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/mini_net_server 8080 30 4      # 端口 空闲超时(秒) IO线程数
```

```bash
curl -i http://127.0.0.1:8080/healthz
curl -i -X POST http://127.0.0.1:8080/api/shorten \
     -H 'Content-Type: application/json' -d '{"url":"https://example.com/hello"}'
curl -i http://127.0.0.1:8080/<返回的短码>          # 302 跳转
curl -i http://127.0.0.1:8080/api/stats/<短码>
curl -s http://127.0.0.1:8080/metrics
```

```bash
ctest --test-dir build --output-on-failure            # 38 个单元/集成测试
cmake -B build-asan -DENABLE_ASAN=ON && cmake --build build-asan -j && ctest --test-dir build-asan   # 内存检查
bash scripts/smoke_test.sh 8080                       # 冒烟：HTTP + 1000 并发 + fd 泄漏
python3 tests/api/test_shorturl.py --base-url http://127.0.0.1:8080   # 接口自动化
./build/echo_bench --host 127.0.0.1 --port 8080 --conns 500 --requests 200 --size 256 --mode http
```

## 目录

```
include/mini_net/   公共头文件（EventLoop / EpollPoller / Channel / TcpServer / Buffer /
                    MemoryPool / EventLoopThread(Pool) / HttpParser / HttpServer / ShortUrl*）
src/                实现
tests/              GoogleTest 单元与集成测试（tests/api、tests/perf、tests/chaos 为 Python 测试）
bench/              自写 C++ 压测客户端
docs/               API 契约、架构与决策、周计划、压测结果、踩坑记录、面试准备
scripts/            环境安装、冒烟测试、远程验证、压测
```

## 关键设计权衡（详见 docs/ARCHITECTURE.md）

- **epoll(ET)**：通知少、syscall 少，代价是必须读到 EAGAIN；
- **one loop per thread**：连接读写天然串行、几乎无锁，代价是负载不均；
- **空闲连接用时间戳 + 每秒扫描**：1 万连接只需 1 个定时器；代价是精度受扫描周期限制；
- **对象池**：减少 malloc 次数、内存更可控；**实测单线程不比 glibc tcache 快**，收益在多线程与内存上限可控；
- **写路径**：写缓冲 + EPOLLOUT，因为非阻塞 send 可能只写一半。

## English

**mini-net — a from-scratch C++17 high-concurrency network service.**

No third-party networking or HTTP libraries: a hand-written Reactor on epoll (edge-triggered,
non-blocking fds), one-loop-per-thread IO model with eventfd wakeups, a ring buffer using
`readv` + a 64 KiB stack extra buffer, and a fixed-size object pool for connection objects.
On top of it: an incremental HTTP/1.1 parser (handles partial packets, pipelining) and a URL
shortener service (create / 302 redirect / stats / metrics).

**Verified in CI**: 38 GoogleTest unit & integration tests, plus Release / ASAN+UBSan builds,
a 1000-concurrent-connection smoke test with zero fd leaks, a hand-written C++ load generator,
pytest API tests, Locust scenarios and chaos tests.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/mini_net_server 8080 30 4      # port, idle-timeout(sec), io-threads
```

See `docs/ARCHITECTURE.md` for the design trade-offs and `docs/RESULTS.md` for benchmark data.
