#pragma once
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace mininet {

class EventLoop;

// 一个线程 + 一个 EventLoop。startLoop() 阻塞到 loop 就绪再返回指针。
class EventLoopThread {
public:
    explicit EventLoopThread(std::string name = "io");
    ~EventLoopThread();
    EventLoopThread(const EventLoopThread&) = delete;

    EventLoop* startLoop();
    void stop();

private:
    void threadFunc();

    EventLoop* loop_{nullptr};
    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread thread_;
    std::string name_;
    bool started_{false};
};

}  // namespace mininet
