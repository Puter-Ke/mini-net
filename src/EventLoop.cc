#include "mini_net/EventLoop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "mini_net/Logger.h"

namespace mininet {

EventLoop::EventLoop()
    : poller_(std::make_unique<EpollPoller>()), owner_(std::this_thread::get_id()) {
    // eventfd：跨线程唤醒的"门铃"。没有它，别的线程投递的任务最坏要等一次 poll 超时才执行。
    wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0) {
        LOG_ERROR("eventfd 创建失败：%s", std::strerror(errno));
        return;
    }
    wakeup_channel_ = std::make_unique<Channel>(wakeup_fd_, this);
    wakeup_channel_->setReadCallback([this] { handleWakeup(); });
    wakeup_channel_->enableReading();
}

EventLoop::~EventLoop() {
    if (wakeup_channel_) wakeup_channel_->disableAll();
    wakeup_channel_.reset();
    if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        LOG_ERROR("跨线程访问了只属于某个 EventLoop 的资源（当前线程 != loop 线程）");
    }
}

int EventLoop::nextTimeoutMs() const {
    if (timers_.empty()) return -1;   // 没有定时任务 → 永久阻塞，等 IO 或 eventfd 唤醒
    const auto now = std::chrono::steady_clock::now();
    int best = 1000;
    for (const auto& t : timers_) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.next - now).count();
        best = std::min<int>(best, static_cast<int>(ms < 0 ? 0 : ms));
    }
    return best;
}

void EventLoop::loop() {
    looping_ = true;
    quit_ = false;
    LOG_DEBUG("事件循环开始，线程 %lu", static_cast<unsigned long>(std::hash<std::thread::id>{}(owner_)));
    while (!quit_) {
        active_.clear();
        poller_->wait(active_, nextTimeoutMs());
        // 顺序：IO → 定时任务 → 跨线程任务（IO 最延迟敏感）
        for (Channel* ch : active_) {
            ch->handleEvent(ch->revents());
        }
        runDueTimers();
        doPendingFunctors();
    }
    looping_ = false;
}

void EventLoop::quit() {
    quit_ = true;
    if (!isInLoopThread()) wakeup();   // 否则阻塞在 epoll_wait 的线程永远醒不来
}

void EventLoop::wakeup() {
    if (wakeup_fd_ < 0) return;
    const uint64_t one = 1;
    const ssize_t n = ::write(wakeup_fd_, &one, sizeof one);
    if (n != static_cast<ssize_t>(sizeof one) && errno != EAGAIN) {
        LOG_WARN("wakeup 写入 eventfd 失败：%s", std::strerror(errno));
    }
}

void EventLoop::handleWakeup() {
    uint64_t v = 0;
    // 必须把 8 字节读干净，否则 ET/level 语义下会一直触发
    while (::read(wakeup_fd_, &v, sizeof v) > 0) {
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
    // 两种情况必须唤醒：
    //   1) 别的线程投递任务 —— 本线程可能正阻塞在 epoll_wait
    //   2) 本线程正在执行 pending 队列 —— 新任务要等下一轮，唤醒可以让它马上被处理
    if (!isInLoopThread() || calling_pending_) wakeup();
}

size_t EventLoop::pendingCount() {
    std::lock_guard<std::mutex> lk(mtx_);
    return pending_.size();
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

void EventLoop::doPendingFunctors() {
    std::vector<std::function<void()>> functors;
    calling_pending_ = true;
    {
        // 换到局部变量再执行：锁只保护交换，任务执行期间不持锁
        // （否则任务里再 queueInLoop 就会自己等自己 → 死锁）
        std::lock_guard<std::mutex> lk(mtx_);
        functors.swap(pending_);
    }
    for (auto& f : functors) f();
    calling_pending_ = false;
}

}  // namespace mininet
