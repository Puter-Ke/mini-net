#include "mini_net/EpollPoller.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace mininet {

EpollPoller::EpollPoller() : epfd_(::epoll_create1(EPOLL_CLOEXEC)), events_(kMaxEvents) {
    if (epfd_ < 0) {
        std::perror("epoll_create1");
        std::abort();
    }
}

EpollPoller::~EpollPoller() {
    if (epfd_ >= 0) ::close(epfd_);
}

void EpollPoller::updateChannel(Channel* ch) {
    const int fd = ch->fd();
    epoll_event ev;
    std::memset(&ev, 0, sizeof ev);
    ev.events = ch->events();
    ev.data.ptr = ch;   // poller 用这个指针直接找回 Channel，省掉一次哈希查找

    const bool known = channels_.find(fd) != channels_.end();
    const int op = known ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (::epoll_ctl(epfd_, op, fd, &ev) < 0) {
        // ADD 时如果发现已存在（比如别处也注册过），退化成 MOD 再试一次
        if (op == EPOLL_CTL_ADD && errno == EEXIST) {
            if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0) {
                channels_[fd] = ch;
                ch->setInPoller(true);
                return;
            }
        }
        std::perror("epoll_ctl");
        return;
    }
    channels_[fd] = ch;
    ch->setInPoller(true);
}

void EpollPoller::removeChannel(Channel* ch) {
    const int fd = ch->fd();
    auto it = channels_.find(fd);
    if (it == channels_.end()) return;
    if (::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0) std::perror("epoll_ctl DEL");
    channels_.erase(it);
    ch->setInPoller(false);
}

int EpollPoller::wait(std::vector<Channel*>& active, int timeout_ms) {
    const int n = ::epoll_wait(epfd_, events_.data(), kMaxEvents, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return 0;   // 被信号打断是正常的，重试即可
        std::perror("epoll_wait");
        return -1;
    }
    for (int i = 0; i < n; ++i) {
        auto* ch = static_cast<Channel*>(events_[i].data.ptr);
        ch->setRevents(events_[i].events);
        active.push_back(ch);
    }
    return n;
}

}  // namespace mininet
