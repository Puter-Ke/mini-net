#pragma once
#include <sys/epoll.h>

#include <unordered_map>
#include <vector>

#include "mini_net/Channel.h"

namespace mininet {

// epoll 的薄封装。面试点：它只管"注册/修改/等待"，不关心业务。
class EpollPoller {
public:
    EpollPoller();
    ~EpollPoller();
    EpollPoller(const EpollPoller&) = delete;
    EpollPoller& operator=(const EpollPoller&) = delete;

    void updateChannel(Channel* ch);   // 自动判断 ADD 还是 MOD
    void removeChannel(Channel* ch);
    int wait(std::vector<Channel*>& active, int timeout_ms);

    static constexpr int kMaxEvents = 1024;

private:
    int epfd_;
    std::unordered_map<int, Channel*> channels_;   // fd -> Channel，用于判断 ADD/MOD
    std::vector<epoll_event> events_;
};

}  // namespace mininet
