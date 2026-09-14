#include "mini_net/Channel.h"

#include "mini_net/EventLoop.h"

namespace mininet {

Channel::Channel(int fd, EventLoop* loop) : fd_(fd), loop_(loop) {}

Channel::~Channel() {
    // 关键：先摘掉再析构。否则 epoll 里的 data.ptr 会指向已释放对象（悬垂指针）
    disableAll();
}

void Channel::enableReading() {
    events_ |= (EPOLLIN | EPOLLRDHUP | EPOLLET);   // 边缘触发，一次事件要把数据读干净
    update();
}

void Channel::enableWriting() {
    events_ |= EPOLLOUT;
    writing_ = true;
    update();
}

void Channel::disableWriting() {
    events_ &= ~static_cast<uint32_t>(EPOLLOUT);
    writing_ = false;
    update();
}

void Channel::disableReading() {
    events_ &= ~static_cast<uint32_t>(EPOLLIN | EPOLLRDHUP);
    update();
}

void Channel::disableAll() {
    events_ = 0;
    if (in_poller_) loop_->removeChannel(this);
}

void Channel::update() {
    if (loop_ != nullptr) loop_->updateChannel(this);
}

void Channel::handleEvent(uint32_t revents) {
    revents_ = revents;

    // 先把回调拷到栈上：回调内部可能关闭连接并析构 this，
    // 那样后面再读成员就是 use-after-free（拷一份代价很小，换来确定性）
    const Callback read_cb = read_cb_;
    const Callback write_cb = write_cb_;
    const Callback close_cb = close_cb_;

    if (revents & (EPOLLHUP | EPOLLERR)) {
        if (close_cb) close_cb();
        return;
    }
    if (revents & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (read_cb) read_cb();
    }
    if (revents & EPOLLOUT) {
        if (write_cb) write_cb();
    }
}

}  // namespace mininet
