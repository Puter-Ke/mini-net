#include "mini_net/TcpServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "mini_net/Buffer.h"
#include "mini_net/Logger.h"

namespace mininet {

// 一条连接的落地点：fd + Channel + 读缓冲 + 最后活跃时间
// 面试点：为什么记时间戳而不是给每条连接开一个定时器？
//   1 万条连接 = 1 万个定时器对象，内存和更新成本都高；
//   周期扫描 + 时间戳比较是 O(n) 但常数极小，且只需要一个定时器。
struct TcpServer::Conn {
    Conn(int fd, EventLoop* loop)
        : fd(fd), channel(fd, loop), last_active(std::chrono::steady_clock::now()) {}
    int fd;
    Channel channel;
    Buffer in;
    std::chrono::steady_clock::time_point last_active;
    uint64_t recv_bytes{0};
};

TcpServer::TcpServer(EventLoop* loop, uint16_t port) : loop_(loop), port_(port) {}

TcpServer::~TcpServer() {
    if (idle_timer_id_ >= 0) loop_->cancelTimer(idle_timer_id_);
    if (stats_timer_id_ >= 0) loop_->cancelTimer(stats_timer_id_);
    for (auto& kv : conns_) ::close(kv.first);
    conns_.clear();
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

void TcpServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        LOG_ERROR("socket 创建失败: %s", std::strerror(errno));
        return;
    }
    int on = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        LOG_ERROR("bind %u 失败: %s", port_, std::strerror(errno));
        return;
    }
    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        LOG_ERROR("listen 失败: %s", std::strerror(errno));
        return;
    }

    accept_channel_ = std::make_unique<Channel>(listen_fd_, loop_);
    accept_channel_->setReadCallback([this] { handleAccept(); });
    accept_channel_->enableReading();

    // 周期任务：每秒扫一次空闲连接，每 5 秒打一次统计
    idle_timer_id_ = loop_->runEvery(1000, [this] { sweepIdle(); });
    stats_timer_id_ = loop_->runEvery(5000, [this] { reportStats(); });

    LOG_INFO("服务启动：0.0.0.0:%u，空闲超时 %d 秒", port_, idle_timeout_s_);
}

void TcpServer::handleAccept() {
    // ET 模式下必须一口气 accept 到 EAGAIN
    while (true) {
        sockaddr_in peer;
        socklen_t len = sizeof peer;
        const int cfd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) {
                LOG_ERROR("fd 耗尽，accept 失败（连接数 %zu）", conns_.size());
                return;
            }
            LOG_ERROR("accept4 失败: %s", std::strerror(errno));
            return;
        }

        int on = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);

        auto conn = std::make_shared<Conn>(cfd, loop_);
        conn->channel.setReadCallback([this, cfd] { handleReadable(cfd); });
        conn->channel.setCloseCallback([this, cfd] { closeConn(cfd, "对端关闭或出错"); });
        conns_[cfd] = conn;
        conn->channel.enableReading();
        ++conn_count_;
        LOG_INFO("新连接 fd=%d 来自 %s:%u（在线 %zu，累计 %llu）", cfd, ip, ntohs(peer.sin_port),
                 conns_.size(), static_cast<unsigned long long>(conn_count_));
        if (on_connection_) on_connection_(cfd, true);
    }
}

void TcpServer::handleReadable(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    auto conn = it->second;

    while (true) {
        int saved = 0;
        const ssize_t n = conn->in.readFd(fd, &saved);
        if (n > 0) {
            conn->last_active = std::chrono::steady_clock::now();   // 刷新活跃时间
            conn->recv_bytes += static_cast<uint64_t>(n);
            recv_bytes_ += static_cast<uint64_t>(n);
            if (on_message_) on_message_(fd, conn->in.peek(), static_cast<size_t>(n));
            conn->in.retrieve(static_cast<size_t>(n));   // M5 要留给协议解析器
            continue;   // ET：继续读到 EAGAIN
        }
        if (n == 0) {
            closeConn(fd, "对端正常关闭");
            return;
        }
        if (saved == EAGAIN || saved == EWOULDBLOCK) return;
        if (saved == EINTR) continue;
        LOG_WARN("fd=%d 读失败: %s", fd, std::strerror(saved));
        closeConn(fd, "读错误");
        return;
    }
}

void TcpServer::closeConn(int fd, const char* reason) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    auto conn = it->second;
    conns_.erase(it);            // 先摘花名册
    conn->channel.disableAll();  // 再从 poller 摘掉
    ::close(fd);
    LOG_INFO("关闭连接 fd=%d（%s，在线 %zu）", fd, reason, conns_.size());
    if (on_connection_) on_connection_(fd, false);
}

void TcpServer::sweepIdle() {
    if (idle_timeout_s_ <= 0) return;
    const auto now = std::chrono::steady_clock::now();

    std::vector<int> expired;
    for (auto& kv : conns_) {
        const auto idle_s = std::chrono::duration_cast<std::chrono::seconds>(now - kv.second->last_active).count();
        if (idle_s >= idle_timeout_s_) expired.push_back(kv.first);
    }
    for (int fd : expired) {
        ++closed_idle_;
        closeConn(fd, "空闲超时");
    }
    if (!expired.empty()) {
        LOG_INFO("本轮清理空闲连接 %zu 条（累计清理 %llu 条）", expired.size(),
                 static_cast<unsigned long long>(closed_idle_));
    }
}

void TcpServer::reportStats() {
    LOG_INFO("统计：在线 %zu，累计连接 %llu，累计接收 %.2f MB，空闲清理 %llu",
             conns_.size(), static_cast<unsigned long long>(conn_count_),
             static_cast<double>(recv_bytes_) / 1024.0 / 1024.0,
             static_cast<unsigned long long>(closed_idle_));
}

}  // namespace mininet
