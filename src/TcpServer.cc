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

#include "mini_net/Buffer.h"

namespace mininet {

// 每条连接的私有状态：持有 Channel 和读缓冲。
// 面试点：这个结构体就是"连接"这个概念的落地点（连接 = fd + 状态 + 缓冲 + 归属的 loop）。
struct TcpServer::Conn {
    Conn(int fd, EventLoop* loop) : fd(fd), channel(fd, loop) {}
    int fd;
    Channel channel;
    Buffer in;          // 读缓冲：解决粘包/拆包
    uint64_t recv_bytes{0};
};

TcpServer::TcpServer(EventLoop* loop, uint16_t port) : loop_(loop), port_(port) {}

TcpServer::~TcpServer() {
    // 先关所有连接，再关监听 fd
    for (auto& kv : conns_) ::close(kv.first);
    conns_.clear();
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

void TcpServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        std::perror("socket");
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
        std::perror("bind");
        return;
    }
    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        std::perror("listen");
        return;
    }

    accept_channel_ = std::make_unique<Channel>(listen_fd_, loop_);
    accept_channel_->setReadCallback([this] { handleAccept(); });
    accept_channel_->enableReading();
    std::printf("[server] listening on 0.0.0.0:%u\n", port_);
}

void TcpServer::handleAccept() {
    // ET 模式下必须一口气 accept 到 EAGAIN，否则新连接会一直卡在半连接队列里
    while (true) {
        sockaddr_in peer;
        socklen_t len = sizeof peer;
        const int cfd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;   // 全接完了，正常退出
            if (errno == EINTR) continue;                          // 被信号打断，重试
            if (errno == EMFILE || errno == ENFILE) {               // fd 用光了
                std::perror("[server] accept4: fd exhausted");
                return;
            }
            std::perror("accept4");
            return;
        }

        int on = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);   // 关 Nagle，小包低延迟

        auto conn = std::make_shared<Conn>(cfd, loop_);
        conn->channel.setReadCallback([this, cfd] { handleReadable(cfd); });
        conn->channel.setCloseCallback([this, cfd] { closeConn(cfd); });
        conns_[cfd] = conn;
        conn->channel.enableReading();
        ++conn_count_;
        if (on_connection_) on_connection_(cfd, true);
    }
}

void TcpServer::handleReadable(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    auto conn = it->second;   // 用 shared_ptr 兜住生命周期，回调里不会被误释放

    while (true) {
        int saved = 0;
        const ssize_t n = conn->in.readFd(fd, &saved);
        if (n > 0) {
            conn->recv_bytes += static_cast<uint64_t>(n);
            if (on_message_) on_message_(fd, conn->in.peek(), static_cast<size_t>(n));
            conn->in.retrieve(static_cast<size_t>(n));   // M1 先"读完就扔"；M5 要留给协议解析器
            continue;   // ET 模式：继续读，直到 EAGAIN
        }
        if (n == 0) {
            closeConn(fd);   // 对端正常关闭
            return;
        }
        if (saved == EAGAIN || saved == EWOULDBLOCK) return;   // 读干净了
        if (saved == EINTR) continue;
        closeConn(fd);   // 其他错误，直接关
        return;
    }
}

void TcpServer::closeConn(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    auto conn = it->second;
    conns_.erase(it);          // 先摘出花名册
    conn->channel.disableAll(); // 再从 poller 摘掉（顺序很重要）
    ::close(fd);
    if (on_connection_) on_connection_(fd, false);
}

}  // namespace mininet
