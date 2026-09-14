#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "mini_net/ThreadPool.h"

using namespace mininet;

TEST(ThreadPoolTest, RunsEverySubmittedTask) {
    ThreadPool pool(4);
    std::atomic<int> done{0};
    constexpr int kTotal = 200;

    for (int i = 0; i < kTotal; ++i) {
        pool.submit([&done] { done.fetch_add(1); });
    }
    for (int i = 0; i < 400 && done.load() < kTotal; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(done.load(), kTotal);
}

TEST(ThreadPoolTest, DrainsQueueOnDestruction) {
    std::atomic<int> done{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 50; ++i) pool.submit([&done] { done.fetch_add(1); });
    }   // 析构时应该把队列里的任务干完再退出
    EXPECT_EQ(done.load(), 50);
}
