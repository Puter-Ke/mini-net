#include "mini_net/EventLoopThread.h"

#include <utility>

#include "mini_net/EventLoop.h"
#include "mini_net/Logger.h"

namespace mininet {

EventLoopThread::EventLoopThread(std::string name) : name_(std::move(name)) {}

EventLoopThread::~EventLoopThread() {
    stop();
    if (thread_.joinable()) thread_.join();
}

EventLoop* EventLoopThread::startLoop() {
    std::unique_lock<std::mutex> lk(mtx_);
    if (!started_) {
        thread_ = std::thread([this] { threadFunc(); });
        started_ = true;
    }
    // 等 loop 在自己的线程里创建完成（EventLoop 必须在它自己的线程里构造）
    cv_.wait(lk, [this] { return loop_ != nullptr; });
    return loop_;
}

void EventLoopThread::threadFunc() {
    EventLoop loop;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        loop_ = &loop;
    }
    cv_.notify_one();
    loop.loop();          // 阻塞在这里直到 stop()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        loop_ = nullptr;
    }
}

void EventLoopThread::stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (loop_ != nullptr) loop_->quit();
    }
    if (thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

}  // namespace mininet
