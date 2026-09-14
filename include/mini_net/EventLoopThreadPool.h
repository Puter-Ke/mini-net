#pragma once
#include <memory>
#include <string>
#include <vector>

#include "mini_net/EventLoopThread.h"

namespace mininet {

class EventLoop;

// IO 线程池：一个 base loop 负责 accept，N 个 sub loop 负责读写（round-robin 分派）
// 面试点：为什么不把所有连接都丢给线程池（而不是绑定到 loop）？
//   绑定之后每条连接的读写天然串行、几乎不需要锁；
//   丢线程池则每次读写都要考虑并发，且线程切换成本高。
class EventLoopThreadPool {
public:
    explicit EventLoopThreadPool(EventLoop* base_loop, std::string name = "io");
    ~EventLoopThreadPool();
    EventLoopThreadPool(const EventLoopThreadPool&) = delete;

    void setThreadNum(int n) { thread_num_ = n < 0 ? 0 : n; }
    int threadNum() const { return thread_num_; }

    void start();              // 创建线程并启动所有 sub loop
    EventLoop* nextLoop();     // round-robin；线程数为 0 时返回 base loop

private:
    EventLoop* base_loop_;
    std::string name_;
    int thread_num_{0};
    size_t next_{0};
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

}  // namespace mininet
