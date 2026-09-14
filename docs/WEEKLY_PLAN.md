# mini-net 8 周计划

> 节奏：每周 12–15 小时（每天 1.5–2h + 周末各 4h）。
> 铁律：**每周结束时必须能运行、能演示**。宁可砍功能，不要留编译不过的半成品。

## 第 0 周（本周，半天）：环境
- `wsl --install -d Ubuntu` → 重启 → 建用户 → `bash scripts/env-setup.sh`
- 装 VS Code + WSL 扩展（在 WSL 里打开 `~/projects/mini-net`，不要放 `/mnt/c`，文件 IO 慢 10 倍）
- `git init` 并推到 GitHub（私有也行）
- **验收**：`cmake -B build && cmake --build build -j` 能编出 `mini_net_server` 和 `mininet_tests`（此时 TODO 未实现，跑起来打印提示属正常）

## 第 1 周：单线程 Reactor + epoll 回显服务器
- 实现 `EpollPoller`：add/mod/del、wait，用 ET（边缘触发）+ 非阻塞 fd
- 实现 `Channel::handleEvent` 分发、`TcpServer` 的 listen/accept（accept 必须循环到 EAGAIN）
- 实现 `EventLoop::loop` 主循环 + 连接关闭清理
- **验收**：
  - `nc 127.0.0.1 8080` 发什么回什么
  - `for i in {1..1000}; do nc ... &` 1000 个空连接不崩、fd 不泄漏（`ls /proc/<pid>/fd | wc -l` 回到基线）
  - `cmake -B build-asan -DENABLE_ASAN=ON` 跑一遍无报错
- **坑清单（记进 docs/PITFALLS.md）**：EAGAIN/EINTR 处理、accept 只调一次导致连接堆积、ET 下漏读、SIGPIPE 直接杀进程（要用 MSG_NOSIGNAL）

## 第 2 周：定时器 + 日志 ✅ 已完成
- [x] `EventLoop::runEvery`：周期任务（IO 派发之后执行，回调里 cancel 自身也安全）
- [x] 空闲连接踢掉：**每个连接记最后活跃时间戳 + 每秒扫描**，而不是给每条连接挂一个定时器
      （1 万连接 = 1 万个定时器对象，内存和刷新成本都高；扫描 O(n) 但常数极小、只要一个定时器）
- [x] 日志：级别过滤 + 时间戳 + 文件行号（`include/mini_net/Logger.h`）
- [x] 统计上报：每 5 秒打印在线连接、累计连接、累计流量、空闲清理数
- [x] 集成测试 `test_idle_timeout.cc`：连上不发数据，3 秒内被服务端关闭（read 返回 0）
- **验收**：`nc 127.0.0.1 8080` 挂着不动，30 秒后被服务端主动关闭，日志里能看到原因
- 说明：`TimerWheel`（时间轮）保留给 M5 的海量一次性定时器（HTTP 读超时），
  空闲连接这种"周期性扫描"场景用时间戳更简单——**面试时这正是"知道两种方案各自代价"的例子**"

## 第 3 周：多线程（one loop per thread）+ 线程池
- main loop 只做 accept，把新连接 round-robin 分给 N 个 sub-loop（`EventLoopThreadPool`）
- 加 eventfd 唤醒：`queueInLoop` + `wakeup` + `doPendingFunctors`（换出局部变量再执行）
- **验收**：
  - 4 线程下压测 QPS ≥ 单线程的 2 倍（把数字写进 docs/RESULTS.md）
  - 跨线程操作 poller 的断言开着跑不出错（证明你没有从别的线程改 epoll）
  - 关掉服务端时无内存泄漏（ASAN）

## 第 4 周：Buffer + 内存池
- `Buffer` 换环形缓冲：读写下标、自动扩容、空间复用
- `readFd` 用 `readv` + 栈上 64KB extrabuf，一次 syscall 读完
- 自己写 `MemoryPool`/对象池（可以先只做定长小对象：连接对象、任务对象）
- **验收**：`perf record -g` 火焰图里 `malloc/free` 占比从初版 X% 降到 <5%；给出前后火焰图对比
- **坑**：扩容导致 `peek()` 指针失效、内存对齐、析构顺序

## 第 5 周：HTTP/1.1 + 短链业务
- 手写 HTTP 请求解析：请求行/头/body、`Content-Length`、keep-alive、（可选）`chunked`
- 业务接口：`POST /api/shorten`（返回短码）、`GET /:code`（302 跳转）、`GET /api/stats/:code`
- 存储：先内存哈希表 + 定期刷盘；有余力换 WAL 追加日志
- **验收**：curl/Postman 全接口通过；短码冲突正确处理；非法请求返回 400 且不崩
- **坑**：粘包/拆包、超大 header、慢速攻击（读超时）、响应序列化

## 第 6 周：压测与优化（产出报告）
- 写 `bench/echo_bench`：非阻塞 connect + epoll，输出 QPS 与 P50/P90/P99/P999
- 做对照实验并全部记录到 `docs/RESULTS.md`：单/多线程、有无内存池、有无 Nagle 关闭、ET vs LT
- 定位一个真实瓶颈并修掉（火焰图 / perf top / `strace -c` 看 syscall 次数）
- **验收**：给出"优化前 → 优化后"表格（QPS、P99、CPU 占用、syscall 次数），并写清每一步的理由

## 第 7 周：自动化测试套件（这一周直接对应软测岗 JD）
- 单元测试：GoogleTest 覆盖 Buffer/TimerWheel/HttpParser，覆盖率 ≥70%（`gcov`/`lcov` 出报告）
- 接口自动化：pytest + requests 封装，数据驱动用例 ≥60 条，allure 报告
- 性能测试：Locust 脚本 + 阈值断言（P99 > 阈值则 CI 失败）
- 混沌/异常测试：随机杀进程、断连接、发畸形包，验证服务自愈不崩
- CI：GitHub Actions 上跑构建 + 单测 + 接口测试 + ASAN 构建，**徽章全绿**
- **验收**：CI 页面截图、覆盖率报告、allure 报告三样都进仓库

## 第 8 周：文档与求职转化
- `docs/ARCHITECTURE.md`：架构图（用 draw.io/mermaid）+ 每个设计决策的"为什么"和"代价"
- README 中英双语（华为 JD 明确写"英语读写流利者优先"，英文 README 是最便宜的证明）
- 写一篇技术博客：《我为什么用环形缓冲重写了 mini-net 的 IO 路径》，贴压测数据
- 简历三条量化描述（见下）+ 面试自查清单过一遍
- **验收**：能在 5 分钟内对着架构图讲完整项目，并被追问 3 层不卡壳

## 简历怎么写（做完再改数字）
- 从零实现 C++17 高并发网络库（约 3k 行，无第三方网络框架）：Reactor + epoll(ET) + one-loop-per-thread + 环形缓冲 + 内存池，支持 10k 并发连接。
- 自写压测工具做对照实验，单机 QPS 从 A 提升到 B（+X%），P99 从 C ms 降到 D ms，火焰图显示 malloc/free 占比由 X% 降至 <5%。
- 配套自动化测试（GoogleTest 覆盖率 X% + pytest 60 用例 + Locust 阈值断言 + 混沌用例）接入 GitHub Actions，CI 全绿；期间发现并修复 3 个真实缺陷（连接泄漏/ET 漏读/定时器竞态）。

## 面试自查清单（第 8 周过一遍，答不上来就回去补代码注释）
1. 为什么用 ET？ET 下漏读会怎样？怎么保证读干净？
2. epoll 惊群怎么解决？EPOLLEXCLUSIVE 是什么？
3. one loop per thread vs 线程池处理 IO，各自代价？
4. 为什么需要 eventfd 唤醒？不用会怎样？
5. 时间轮 vs 最小堆定时器，谁更适合海量连接？
6. Buffer 为什么不用 std::string 拼接？环形缓冲扩容时指针失效问题？
7. 内存池相比 malloc 省在哪（系统调用、碎片、缓存局部性）？
8. TCP 粘包的本质原因？你的 HTTP 解析怎么处理半包？
9. 你的服务能抗多少人？瓶颈在哪一环？你怎么知道的（证据）？
10. 如果连接数再涨 10 倍，你的方案哪里先崩？下一步怎么改？
