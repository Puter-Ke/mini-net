#include <gtest/gtest.h>

#include "mini_net/TimerWheel.h"

using namespace mininet;

// TODO(M2): 定时器测试。用假的"时间推进"而不是 sleep，否则 CI 会很慢且不稳
TEST(TimerWheelTest, Placeholder) {
    TimerWheel wheel(100, 60);
    bool fired = false;
    wheel.addTimer(100, [&fired] { fired = true; });
    for (int i = 0; i < 2; ++i) wheel.tick();
    EXPECT_TRUE(fired);
}
