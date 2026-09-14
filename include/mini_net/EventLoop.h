#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "mini_net/EpollPoller.h"

namespace mininet {

// 事件循环（one loop per thread）
// M1：单线程跑通；M3 你要补上 eventfd 唤醒 + 跨线程投递。
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    void loop();
    void quit() { quit_ = true; }
    bool isInLoopThread() const { return owner_ == std::this_thread::get_id(); }

    void runInLoop(std::function<void()> cb);
    void queueInLoop(std::function<void()> cb);

    void updateChannel(Channel* ch) { poller_->updateChannel(ch); }
    void removeChannel(Channel* ch) { poller_->removeChannel(ch); }

    // 每秒调用多少次由 timeout_ms 决定；M2 的定时器挂在这里
    int pollTimeoutMs() const { return poll_timeout_ms_; }

private:
    void doPendingFunctors();

    std::unique_ptr<EpollPoller> poller_;
    std::vector<Channel*> active_;
    std::atomic<bool> quit_{false};
    bool looping_{false};
    std::thread::id owner_;
    int poll_timeout_ms_{100};   // 先用 100ms 轮询，M3 加 eventfd 后可以改成 -1 永久阻塞
    std::vector<std::function<void()>> pending_;
    std::mutex mtx_;
};

}  // namespace mininet
