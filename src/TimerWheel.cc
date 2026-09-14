#include "mini_net/TimerWheel.h"

#include <algorithm>

namespace mininet {

TimerWheel::TimerWheel(size_t tick_ms, size_t wheel_size)
    : tick_ms_(tick_ms == 0 ? 100 : tick_ms), wheel_size_(std::max<size_t>(wheel_size, 1)) {}

TimerWheel::~TimerWheel() = default;

TimerWheel::TimerId TimerWheel::addTimer(int timeout_ms, TimerCallback cb) {
    const int ticks = std::max(1, timeout_ms / static_cast<int>(tick_ms_));
    const TimerId id = next_id_++;
    entries_.push_back(Entry{id, ticks, std::move(cb)});
    return id;
}

void TimerWheel::cancelTimer(TimerId id) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [id](const Entry& e) { return e.id == id; }),
                   entries_.end());
}

void TimerWheel::tick() {
    std::vector<Entry> fired;
    std::vector<Entry> keep;
    keep.reserve(entries_.size());
    for (auto& e : entries_) {
        if (--e.remaining_ticks <= 0) {
            fired.push_back(std::move(e));   // 回调里可能再注册定时器，先收集再触发
        } else {
            keep.push_back(std::move(e));
        }
    }
    entries_.swap(keep);
    for (auto& e : fired) e.cb();
    // TODO(M2)：换成 slots_ + 游标 current_slot_，O(1) 插入/删除；并考虑回调里 cancel 自身
}

}  // namespace mininet
