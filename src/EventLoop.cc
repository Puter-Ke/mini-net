#include "mini_net/EventLoop.h"

#include <cstdio>

namespace mininet {

EventLoop::EventLoop() : poller_(std::make_unique<EpollPoller>()), owner_(std::this_thread::get_id()) {}

EventLoop::~EventLoop() = default;

void EventLoop::loop() {
    looping_ = true;
    quit_ = false;
    while (!quit_) {
        active_.clear();
        const int n = poller_->wait(active_, poll_timeout_ms_);
        (void)n;
        // 先派发 IO，再执行跨线程任务
        for (Channel* ch : active_) {
            // 注意：handleEvent 里可能关闭并销毁 Channel，之后不能再用 ch
            ch->handleEvent(ch->revents());
        }
        doPendingFunctors();
    }
    looping_ = false;
}

void EventLoop::runInLoop(std::function<void()> cb) {
    if (isInLoopThread()) {
        cb();
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(std::function<void()> cb) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_.push_back(std::move(cb));
    }
    // TODO(M3)：这里必须 wakeup() 唤醒可能正阻塞在 epoll_wait 的 loop 线程，
    // 否则任务要等到下一次超时（现在最长 100ms）才被执行。
}

void EventLoop::doPendingFunctors() {
    // 关键技巧：把 pending_ 换到局部变量再执行，缩小锁的范围
    std::vector<std::function<void()>> functors;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        functors.swap(pending_);
    }
    for (auto& f : functors) f();
}

}  // namespace mininet
