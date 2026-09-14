#include <signal.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mini_net/EventLoop.h"
#include "mini_net/HttpServer.h"
#include "mini_net/Logger.h"
#include "mini_net/ShortUrlApp.h"

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);   // 不忽略 SIGPIPE：对端关闭后继续写会杀掉进程

    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1])) : 8080;
    const int idle_s = argc > 2 ? std::stoi(argv[2]) : 30;
    const int threads = argc > 3 ? std::stoi(argv[3]) : 4;

    mininet::setLogLevel(mininet::LogLevel::Info);

    mininet::EventLoop loop;
    mininet::HttpServer server(&loop, port);
    server.setThreadNum(threads);
    server.setIdleTimeoutSeconds(idle_s);

    mininet::ShortUrlApp app("127.0.0.1:" + std::to_string(port));
    app.setMetricsSource([&server] { return server.buildMetrics(); });
    server.setMetricsProvider([&app](std::string& out) { app.appendMetrics(out); });
    server.setHandler([&app](const mininet::HttpRequest& req, mininet::HttpResponse* resp) {
        app.handle(req, resp);
    });

    server.start();
    LOG_INFO("mini-net 短链服务就绪：http://127.0.0.1:%u（IO 线程 %d，空闲超时 %d 秒）", port, threads,
             idle_s);
    LOG_INFO("接口：GET /healthz  GET /metrics  POST /api/shorten  GET /{code}  GET /api/stats/{code}");
    loop.loop();

    LOG_INFO("退出：累计请求 %llu，累计连接 %llu", static_cast<unsigned long long>(server.totalRequests()),
             static_cast<unsigned long long>(server.tcp()->totalConnections()));
    return 0;
}
