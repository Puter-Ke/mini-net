#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "mini_net/Channel.h"
#include "mini_net/EventLoop.h"

using namespace mininet;

// 集成测试：往管道里写一个字节，事件循环应该派发到 Channel 的读回调并退出
TEST(EventLoopTest, DispatchesReadableEvent) {
    EventLoop loop;

    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);

    Channel ch(fds[0], &loop);
    int fired = 0;
    ch.setReadCallback([&] {
        char buf[16];
        const ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n > 0) ++fired;
        loop.quit();
    });
    ch.enableReading();

    ASSERT_EQ(::write(fds[1], "x", 1), 1);
    loop.loop();

    EXPECT_EQ(fired, 1);
    ::close(fds[0]);
    ::close(fds[1]);
}
