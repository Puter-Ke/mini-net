#include <signal.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mini_net/EventLoop.h"
#include "mini_net/Logger.h"
#include "mini_net/TcpServer.h"

int main(int argc, char** argv) {
    // 不忽略 SIGPIPE，对端关闭后继续 send 会直接杀掉进程
    ::signal(SIGPIPE, SIG_IGN);

    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1])) : 8080;
    const int idle_s = argc > 2 ? std::stoi(argv[2]) : 30;
    const int threads = argc > 3 ? std::stoi(argv[3]) : 4;

    mininet::setLogLevel(mininet::LogLevel::Info);

    mininet::EventLoop loop;
    mininet::TcpServer server(&loop, port);
    server.setIdleTimeoutSeconds(idle_s);
    server.setThreadNum(threads);

    // M1-M4 用回显业务；M5 换成 HTTP 解析 + 短链路由
    server.setMessageCallback([](int fd, const char* data, size_t len) {
        const ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0) LOG_ERROR("send 失败 fd=%d：%s", fd, std::strerror(errno));
        // TODO(M4)：send 可能只写一部分，需要写缓冲 + 关注 EPOLLOUT
    });

    server.start();
    LOG_INFO("mini-net 就绪：端口 %u，IO 线程 %d，空闲超时 %d 秒", port, threads, idle_s);
    loop.loop();

    LOG_INFO("退出：累计连接 %llu，累计接收 %.2f MB",
             static_cast<unsigned long long>(server.totalConnections()),
             static_cast<double>(server.totalReceivedBytes()) / 1024.0 / 1024.0);
    return 0;
}
