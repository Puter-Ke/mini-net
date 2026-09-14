#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "mini_net/EventLoop.h"
#include "mini_net/TcpServer.h"

using namespace mininet;

namespace {

int connectTo(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool readExactly(int fd, std::string& out, size_t n) {
    out.clear();
    char buf[256];
    while (out.size() < n) {
        const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
        if (r <= 0) return false;
        out.append(buf, static_cast<size_t>(r));
    }
    return true;
}

}  // namespace

// 多线程服务器：4 个 IO 线程 + 20 个并发客户端，验证分派到不同 loop 后回显依然正确
TEST(TcpServerTest, MultithreadedEchoAcrossLoops) {
    constexpr uint16_t kPort = 19192;
    constexpr int kClients = 20;

    EventLoop loop;
    TcpServer server(&loop, kPort);
    server.setThreadNum(4);
    server.setIdleTimeoutSeconds(0);   // 本测试不测超时
    server.setMessageCallback([](int fd, uint64_t /*conn_id*/, const char* data, size_t len) {
        ::send(fd, data, len, MSG_NOSIGNAL);
    });
    server.start();

    std::thread loop_thread([&] { loop.loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::atomic<int> ok{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back([&ok, i] {
            const int fd = connectTo(kPort);
            if (fd < 0) return;
            const std::string msg = "hello-from-client-" + std::to_string(i);
            if (::send(fd, msg.data(), msg.size(), 0) != static_cast<ssize_t>(msg.size())) {
                ::close(fd);
                return;
            }
            std::string got;
            if (readExactly(fd, got, msg.size()) && got == msg) ok.fetch_add(1);
            ::close(fd);
        });
    }
    for (auto& c : clients) c.join();

    EXPECT_EQ(ok.load(), kClients);
    EXPECT_EQ(server.totalConnections(), static_cast<uint64_t>(kClients));

    loop.quit();
    loop_thread.join();
}
