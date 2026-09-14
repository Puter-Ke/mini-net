# 踩坑记录（面试的弹药）

> 每次 debug 超过 30 分钟，就在这里加一条：现象 / 定位手段 / 根因 / 修法。
> 面试时"我遇到过 XXX" 比背答案有说服力得多。

## 模板
### YYYY-MM-DD 现象一句话
- 定位手段：gdb / strace -c / perf record -g / ASAN 报错原文
- 根因：
- 修法：
- 数据：修前 QPS / 修后 QPS

## 已知常见坑（自己踩到时把细节补上）
1. ET 模式下 `accept` 只调一次 → 半连接队列堆积，客户端超时
2. 忘记处理 `EINTR` → 信号打断 syscall 直接当错误返回
3. 写 socket 未处理 `SIGPIPE` → 对端关闭直接杀进程（用 `MSG_NOSIGNAL` 或忽略信号）
4. `epoll_wait` 返回的 `epoll_event.data.ptr` 悬垂指针（Channel 已析构）
5. 定时器回调里删除定时器 → 迭代器失效 / 递归 tick
6. `readv` 扩容后缓存 `peek()` 指针 → use-after-free
