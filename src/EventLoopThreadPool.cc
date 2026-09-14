#include "mini_net/EventLoopThreadPool.h"

#include "mini_net/EventLoop.h"
#include "mini_net/Logger.h"

namespace mininet {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop, std::string name)
    : base_loop_(base_loop), name_(std::move(name)) {}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::start() {
    if (thread_num_ <= 0) {
        LOG_INFO("IO 线程池：单线程模式（base loop 兼做读写）");
        return;
    }
    threads_.reserve(static_cast<size_t>(thread_num_));
    loops_.reserve(static_cast<size_t>(thread_num_));
    for (int i = 0; i < thread_num_; ++i) {
        auto t = std::make_unique<EventLoopThread>(name_ + "-" + std::to_string(i));
        loops_.push_back(t->startLoop());
        threads_.push_back(std::move(t));
    }
    LOG_INFO("IO 线程池已启动：%d 个 sub loop", thread_num_);
}

EventLoop* EventLoopThreadPool::nextLoop() {
    if (loops_.empty()) return base_loop_;
    EventLoop* loop = loops_[next_ % loops_.size()];
    ++next_;
    return loop;
}

}  // namespace mininet
