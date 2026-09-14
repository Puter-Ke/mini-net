#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "mini_net/EpollPoller.h"

namespace mininet {

// 事件循环（one loop per thread）
// M1：单线程跑通；M2：加周期定时器；M3：补 eventfd 唤醒 + 跨线程投递
class EventLoop {
public:
    using TimerCallback = std::function<void()>;

    EventLoop();
    ~EventLoop();

    void loop();
    void quit() { quit_ = true; }
    bool isInLoopThread() const { return owner_ == std::this_thread::get_id(); }

    void runInLoop(std::function<void()> cb);
    void queueInLoop(std::function<void()> cb);

    // 周期任务：每隔 interval_ms 执行一次，返回 id 可用于取消
    // 用途：空闲连接扫描、统计打印、心跳
    int runEvery(int interval_ms, TimerCallback cb);
    void cancelTimer(int id);

    void updateChannel(Channel* ch) { poller_->updateChannel(ch); }
    void removeChannel(Channel* ch) { poller_->removeChannel(ch); }

private:
    struct TimedTask {
        int id;
        int interval_ms;
        std::chrono::steady_clock::time_point next;
        TimerCallback cb;
    };

    void runDueTimers();
    void doPendingFunctors();

    std::unique_ptr<EpollPoller> poller_;
    std::vector<Channel*> active_;
    std::vector<TimedTask> timers_;
    int next_timer_id_{1};
    std::atomic<bool> quit_{false};
    bool looping_{false};
    std::thread::id owner_;
    int poll_timeout_ms_{100};   // M3 加 eventfd 后可以改成 -1
    std::vector<std::function<void()>> pending_;
    std::mutex mtx_;
};

}  // namespace mininet
