#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "mini_net/EventLoop.h"
#include "mini_net/TcpServer.h"

using namespace mininet;

// 集成测试：连上之后什么都不发，服务端应该在超时后主动关闭这条连接
TEST(TcpServerTest, ClosesIdleConnection) {
    constexpr uint16_t kPort = 19191;

    EventLoop loop;
    TcpServer server(&loop, kPort);
    server.setIdleTimeoutSeconds(1);
    server.setMessageCallback([](int, uint64_t, const char*, size_t) {});
    server.start();

    std::thread loop_thread([&] { loop.loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // 等 loop 起来

    const int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(cfd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(::connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);

    // 什么都不发，等空闲超时（1 秒扫描 + 1 秒超时，给足余量）
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    char buf[1];
    const ssize_t n = ::read(cfd, buf, sizeof buf);
    EXPECT_EQ(n, 0) << "服务端没有踢掉空闲连接（read 应返回 0 表示对端已关闭）";

    ::close(cfd);
    loop.quit();
    loop_thread.join();
}
