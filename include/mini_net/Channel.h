#pragma once
#include <sys/epoll.h>

#include <cstdint>
#include <functional>

namespace mininet {

class EventLoop;

// 一个 fd 的"档案"：关注什么事件 + 事件来了叫谁
// 面试点：Channel 不拥有 fd，也不负责 close，它只负责"登记与派发"。
class Channel {
public:
    using Callback = std::function<void()>;

    Channel(int fd, EventLoop* loop);
    ~Channel();   // 析构时自动从 poller 摘掉，避免 poller 里留下悬垂指针

    int fd() const { return fd_; }
    uint32_t events() const { return events_; }
    uint32_t revents() const { return revents_; }
    void setRevents(uint32_t r) { revents_ = r; }
    bool inPoller() const { return in_poller_; }
    void setInPoller(bool v) { in_poller_ = v; }

    void setReadCallback(Callback cb) { read_cb_ = std::move(cb); }
    void setCloseCallback(Callback cb) { close_cb_ = std::move(cb); }

    // 打开/关闭关注的事件，并同步到 epoll
    void enableReading();
    void disableReading();
    void disableAll();

    // 由 EpollPoller 在 epoll_wait 返回后调用
    void handleEvent(uint32_t revents);

private:
    void update();

    int fd_;
    EventLoop* loop_;
    uint32_t events_{0};
    uint32_t revents_{0};
    bool in_poller_{false};
    Callback read_cb_;
    Callback close_cb_;
};

}  // namespace mininet
