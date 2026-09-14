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

    // 对端关闭 / 出错，统一交给 close 回调处理
    if (revents & (EPOLLHUP | EPOLLERR)) {
        if (close_cb_) close_cb_();
        return;
    }
    // EPOLLRDHUP：对端关掉写方向（半关闭），和可读一起处理
    if (revents & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (read_cb_) read_cb_();
    }
    // 注意：M1 只在读回调里直接写回（回显），不要在这里再调 write_cb_，
    // 因为读回调可能已经把这条连接关掉并释放了 Channel（悬垂指针）。
}

}  // namespace mininet
