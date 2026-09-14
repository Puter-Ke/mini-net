#include <signal.h>
#include <sys/socket.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "mini_net/EventLoop.h"
#include "mini_net/TcpServer.h"

int main(int argc, char** argv) {
    // 关键：不忽略 SIGPIPE，对端关闭后继续 write 会直接杀掉进程
    ::signal(SIGPIPE, SIG_IGN);

    uint16_t port = 8080;
    if (argc > 1) port = static_cast<uint16_t>(std::stoi(argv[1]));

    mininet::EventLoop loop;
    mininet::TcpServer server(&loop, port);

    server.setMessageCallback([](int fd, const char* data, size_t len) {
        // M1：回显。用 MSG_NOSIGNAL 双保险，避免 SIGPIPE
        const ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0) std::perror("[server] send");
        // TODO(M1 之后)：send 可能只写一部分（返回 < len），要加写缓冲 + EPOLLOUT
    });

    server.setConnectionCallback([](int fd, bool connected) {
        std::printf("[server] fd=%d %s (alive=%zu)\n", fd, connected ? "connected" : "closed",
                    static_cast<size_t>(0));
    });

    server.start();
    std::printf("mini-net running, ctrl+c to stop\n");
    loop.loop();
    return 0;
}
