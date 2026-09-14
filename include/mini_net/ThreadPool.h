#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace mininet {

// TODO(M3): 线程池。注意面试必问：
//  - 任务队列满了怎么办（无界队列的 OOM 风险 / 有界队列的拒绝策略）
//  - 如何优雅停止（stop_ + notify_all + join）
//  - 能不能不做内存分配就提交任务（任务对象池）
class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();

    void submit(std::function<void()> task);
    size_t size() const { return workers_.size(); }

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_{false};
};

}  // namespace mininet
