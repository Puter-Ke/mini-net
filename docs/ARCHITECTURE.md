# mini-net 架构与设计决策

> 每个模块回答三件事：**解决什么问题 / 为什么这样设计 / 代价是什么**。
> 面试官真正考察的是第三点。

## 分层与数据流

```
                              ┌──────────────────────────────┐
   客户端 ──TCP──▶ listen fd  │ 主线程 EventLoop（base loop）  │
                              │  AcceptorChannel → accept4    │
                              └───────────────┬──────────────┘
                                              │ round-robin 分派
                    ┌─────────────────────────┼─────────────────────────┐
                    ▼                         ▼                         ▼
        ┌────────────────────┐    ┌────────────────────┐    ┌────────────────────┐
        │ IO 线程 1 EventLoop │    │ IO 线程 2 EventLoop │    │ IO 线程 N EventLoop │
        │  epoll_wait         │    │                     │    │                     │
        │  Channel::handleEvent│   │                     │    │                     │
        └─────────┬──────────┘    └────────────────────┘    └────────────────────┘
                  │ 可读
                  ▼
        TcpServer::handleReadable → Buffer::readFd(readv) → on_message_
                  │
                  ▼
        HttpServer::onMessage → HttpParser(增量状态机) → ShortUrlApp::handle
                  │
                  ▼
        HttpResponse::serialize → TcpServer::send(fd, ...)  （写缓冲 + EPOLLOUT）

    业务线程池 ThreadPool：留给 CPU 密集任务（默认不参与 IO 路径，避免上下文切换）
```

## 关键设计决策

| 决策 | 选择 | 为什么 | 代价 |
|---|---|---|---|
| IO 模型 | epoll + **ET** + 非阻塞 fd | 事件通知次数少、syscall 少，适合海量连接 | 必须循环读到 EAGAIN，漏读会"假死"；代码复杂度上升 |
| 线程模型 | **one loop per thread**，连接与线程绑定 | 单条连接的读写天然串行，几乎不需要锁 | 负载不均（热点连接）；线程数不宜过多 |
| 跨线程通信 | `runInLoop` + **eventfd 唤醒** | 任务立即执行（约 1 次 syscall），而不是等 poll 超时 | 多一次 syscall；需要处理 eventfd 读干净 |
| 空闲连接清理 | **每连接记时间戳 + 每秒扫描** | 1 万连接只需 1 个定时器；避免 1 万个定时器对象 | 扫描是 O(n)（常数极小）；精度受扫描周期限制 |
| 缓冲区 | **环形缓冲 + readv + 栈上 64KB extrabuf** | 小包不扩容；取走数据后空间可复用（memmove 复位） | 指针生命周期要小心（扩容后 `peek()` 失效） |
| 连接对象 | **定长对象池**（placement new + 自定义删除器） | 批量申请减少 malloc 次数；同类对象内存连续 | 单线程下不比 glibc tcache 快（实测），收益在多线程与内存可控性 |
| 协议解析 | **增量状态机** | TCP 是字节流，必须能处理半包/粘包/管道化 | 需要保存解析状态，代码分支多 |
| 写路径 | 写缓冲 + EPOLLOUT | 非阻塞 fd 上 send 可能只写一部分 | 需要额外状态与回调 |
| 日志 | 同步 fprintf + 级别过滤 | 实现简单、调试直观 | 高频日志会拖慢 QPS；生产应改异步双缓冲 |

## 已知缺陷与下一步（面试时主动说）

1. `flushWrite` 用 `out.erase(0, n)` 删除已写数据，是 O(n) 拷贝；应改成"读偏移 + 定期压缩"。
2. 连接表 `conns_` 用一把互斥锁保护；连接量再大时应改为分片锁或每线程独立表。
3. 对象池带互斥锁，单线程场景不占优；可改成 thread_local 池或 lock-free 空闲栈。
4. 存储层在内存里（`unordered_map`），进程重启即丢；应加 WAL 或换 Redis/LevelDB。
5. 只支持 HTTP/1.1 且不支持 chunked；未实现 HTTPS/TLS。
6. 缺少优雅重启（SIGTERM → 停止 accept → 排空 → 退出）与限流/熔断。

## 性能测试方法（同机 vs 双机）

- **同机压测**（本项目 CI 用的方式）只能做趋势对比，因为客户端与服务端抢同一批 CPU。
- **双机压测**才有对外可引用的绝对值：客户端跑 C++ 压测程序并绑核，服务端独占机器。
- 每次测前记录：CPU 型号/核数、内存、编译优化级别、是否开 ASAN（ASAN 会慢 2-5 倍，绝不能混测）。
