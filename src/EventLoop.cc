#include "mini_net/EventLoop.h"

#include "mini_net/Logger.h"

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
        // 顺序：IO 优先（延迟敏感）→ 定时任务 → 跨线程任务
        for (Channel* ch : active_) {
            // 注意：handleEvent 里可能关闭并销毁 Channel，之后不能再用 ch
            ch->handleEvent(ch->revents());
        }
        runDueTimers();
        doPendingFunctors();
    }
    looping_ = false;
}

int EventLoop::runEvery(int interval_ms, TimerCallback cb) {
    const int id = next_timer_id_++;
    if (interval_ms < 1) interval_ms = 1;
    timers_.push_back(TimedTask{id, interval_ms,
                                std::chrono::steady_clock::now() + std::chrono::milliseconds(interval_ms),
                                std::move(cb)});
    return id;
}

void EventLoop::cancelTimer(int id) {
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (it->id == id) {
            timers_.erase(it);
            return;
        }
    }
}

void EventLoop::runDueTimers() {
    if (timers_.empty()) return;
    const auto now = std::chrono::steady_clock::now();

    // 先把到期的回调拷出来，再统一执行：
    // 回调里可能会 cancelTimer 或再注册任务，直接遍历 timers_ 会迭代器失效
    std::vector<TimerCallback> due;
    for (auto& t : timers_) {
        if (t.next <= now) {
            t.next = now + std::chrono::milliseconds(t.interval_ms);
            due.push_back(t.cb);
        }
    }
    for (auto& f : due) {
        if (f) f();
    }
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
    // TODO(M3)：必须 wakeup() 唤醒正阻塞在 epoll_wait 的 loop 线程，
    // 否则任务最坏要等一次 poll 超时（当前 100ms）才被执行。
}

void EventLoop::doPendingFunctors() {
    std::vector<std::function<void()>> functors;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        functors.swap(pending_);
    }
    for (auto& f : functors) f();
}

}  // namespace mininet
