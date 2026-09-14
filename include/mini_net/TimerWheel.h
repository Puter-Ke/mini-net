#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace mininet {

// 定时器：先给一个"够用但性能一般"的实现，M2 任务就是把它换成真正的时间轮。
// 面试会问：时间轮 vs 最小堆的取舍？为什么不用 sleep 线程 + 有序链表？
class TimerWheel {
public:
    using TimerCallback = std::function<void()>;
    using TimerId = uint64_t;

    explicit TimerWheel(size_t tick_ms = 100, size_t wheel_size = 60);
    ~TimerWheel();

    TimerId addTimer(int timeout_ms, TimerCallback cb);
    void cancelTimer(TimerId id);
    void tick();   // 由 EventLoop 每 tick_ms 调一次

private:
    struct Entry {
        TimerId id;
        int remaining_ticks;
        TimerCallback cb;
    };

    size_t tick_ms_;
    size_t wheel_size_;
    TimerId next_id_{1};
    std::vector<Entry> entries_;   // TODO(M2): 换成 slots_[wheel_size_] + 每槽链表，做到 O(1)
};

}  // namespace mininet
