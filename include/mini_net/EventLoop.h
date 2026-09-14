#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "mini_net/Channel.h"
#include "mini_net/EpollPoller.h"

namespace mininet {

// 事件循环（one loop per thread）
// M1：单线程跑通；M2：周期定时器；M3：eventfd 唤醒 + 跨线程任务投递
class EventLoop {
public:
    using TimerCallback = std::function<void()>;

    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();
    void quit();
    bool isInLoopThread() const { return owner_ == std::this_thread::get_id(); }
    void assertInLoopThread() const;

    // 跨线程投递：本线程直接执行；其他线程入队 + 唤醒（延迟约 1 次 syscall，而不是等 poll 超时）
    void runInLoop(std::function<void()> cb);
    void queueInLoop(std::function<void()> cb);
    void wakeup();

    int runEvery(int interval_ms, TimerCallback cb);
    void cancelTimer(int id);

    void updateChannel(Channel* ch) { poller_->updateChannel(ch); }
    void removeChannel(Channel* ch) { poller_->removeChannel(ch); }

    size_t pendingCount();

private:
    struct TimedTask {
        int id;
        int interval_ms;
        std::chrono::steady_clock::time_point next;
        TimerCallback cb;
    };

    int nextTimeoutMs() const;   // 下次 poll 该等多久：有定时器按最近一个算，没有就永久阻塞
    void handleWakeup();
    void runDueTimers();
    void doPendingFunctors();

    std::unique_ptr<EpollPoller> poller_;
    std::vector<Channel*> active_;

    int wakeup_fd_{-1};
    std::unique_ptr<Channel> wakeup_channel_;
    std::atomic<bool> calling_pending_{false};

    std::vector<TimedTask> timers_;
    int next_timer_id_{1};

    std::atomic<bool> quit_{false};
    bool looping_{false};
    std::thread::id owner_;
    std::vector<std::function<void()>> pending_;
    std::mutex mtx_;
};

}  // namespace mininet
