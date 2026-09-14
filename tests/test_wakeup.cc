#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "mini_net/EventLoop.h"

using namespace mininet;

// 关键验证：另一个线程投递任务时，loop 必须被 eventfd 立刻唤醒，
// 而不是傻等 poll 超时。（没有 wakeup 的话这里会慢到秒级甚至永久阻塞）
TEST(EventLoopTest, CrossThreadTaskIsWokenUpPromptly) {
    EventLoop loop;

    // 保险绳：注册一个 2 秒的定时任务，万一 wakeup 失效，loop 也不会永久卡死，
    // 测试会以"延迟超阈值"而不是"挂起"的方式失败。
    loop.runEvery(2000, [] {});

    std::thread loop_thread([&] { loop.loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // 让 loop 进入阻塞

    std::atomic<bool> ran{false};
    const auto t0 = std::chrono::steady_clock::now();
    loop.runInLoop([&] {
        ran = true;
        loop.quit();
    });

    for (int i = 0; i < 500 && !ran.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    EXPECT_TRUE(ran.load()) << "跨线程任务没有被执行";
    EXPECT_LT(elapsed_ms, 200) << "跨线程任务延迟过高，eventfd 唤醒可能失效（耗时 " << elapsed_ms << " ms）";

    loop_thread.join();
}
