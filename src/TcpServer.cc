#include "mini_net/TcpServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "mini_net/Buffer.h"
#include "mini_net/Logger.h"

namespace mininet {

namespace {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

// 每条连接的落地点。注意 last_active_ms 是原子的：
// 它在 IO 线程写、在 base loop 的扫描线程读，用普通 int64_t 就是数据竞争（TSAN 会报）。
struct TcpServer::Conn {
    Conn(int fd, EventLoop* loop) : fd(fd), loop(loop), channel(fd, loop), last_active_ms(nowMs()) {}
    int fd;
    EventLoop* loop;
    Channel channel;
    Buffer in;
    std::atomic<int64_t> last_active_ms;
    uint64_t recv_bytes{0};
};

TcpServer::TcpServer(EventLoop* loop, uint16_t port) : loop_(loop), port_(port) {}

TcpServer::~TcpServer() {
    if (idle_timer_id_ >= 0) loop_->cancelTimer(idle_timer_id_);
    if (stats_timer_id_ >= 0) loop_->cancelTimer(stats_timer_id_);
    // 先停掉所有 IO 线程，保证没有人在动 conns_
    if (pool_) pool_->stop();
    std::lock_guard<std::mutex> lk(conns_mtx_);
    for (auto& kv : conns_) ::close(kv.first);
    conns_.clear();
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

void TcpServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        LOG_ERROR("socket 创建失败：%s", std::strerror(errno));
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
        LOG_ERROR("bind %u 失败：%s", port_, std::strerror(errno));
        return;
    }
    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        LOG_ERROR("listen 失败：%s", std::strerror(errno));
        return;
    }

    // 先起 IO 线程池，再把 listen fd 挂上 base loop
    pool_ = std::make_unique<EventLoopThreadPool>(loop_, "io");
    pool_->setThreadNum(thread_num_);
    pool_->start();

    accept_channel_ = std::make_unique<Channel>(listen_fd_, loop_);
    accept_channel_->setReadCallback([this] { handleAccept(); });
    accept_channel_->enableReading();

    idle_timer_id_ = loop_->runEvery(1000, [this] { sweepIdle(); });
    stats_timer_id_ = loop_->runEvery(5000, [this] { reportStats(); });

    LOG_INFO("服务启动：0.0.0.0:%u，IO 线程 %d，空闲超时 %d 秒", port_, pool_->threadNum(),
             idle_timeout_s_);
}

void TcpServer::handleAccept() {
    while (true) {
        sockaddr_in peer;
        socklen_t len = sizeof peer;
        const int cfd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) {
                LOG_ERROR("fd 耗尽，accept 失败");
                return;
            }
            LOG_ERROR("accept4 失败：%s", std::strerror(errno));
            return;
        }

        int on = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        const uint16_t pport = ntohs(peer.sin_port);

        // 关键：连接对象必须在它自己的 IO 线程里创建（Channel 只属于那个 loop）
        EventLoop* io_loop = pool_->nextLoop();
        const std::string ip_str(ip);
        io_loop->runInLoop([this, cfd, io_loop, ip_str, pport] {
            addConnection(cfd, io_loop, ip_str.c_str(), pport);
        });
    }
}

void TcpServer::addConnection(int cfd, EventLoop* io_loop, const char* ip, uint16_t peer_port) {
    auto conn = std::make_shared<Conn>(cfd, io_loop);
    const std::weak_ptr<Conn> weak = conn;   // 用 weak_ptr 打破 "Conn → Channel → 回调 → Conn" 的循环引用

    conn->channel.setReadCallback([this, weak] {
        if (auto c = weak.lock()) handleReadable(c);
    });
    conn->channel.setCloseCallback([this, weak] {
        if (auto c = weak.lock()) closeConn(c, "对端关闭或出错");
    });
    conn->channel.enableReading();

    {
        std::lock_guard<std::mutex> lk(conns_mtx_);
        conns_[cfd] = conn;
    }
    conn_count_.fetch_add(1);
    LOG_INFO("新连接 fd=%d 来自 %s:%u（在线 %zu，累计 %llu，loop=%s）", cfd, ip, peer_port,
             aliveConnections(), static_cast<unsigned long long>(conn_count_.load()),
             io_loop->isInLoopThread() ? "本线程" : "其他线程");
    if (on_connection_) on_connection_(cfd, true);
}

void TcpServer::handleReadable(const std::shared_ptr<Conn>& conn) {
    const int fd = conn->fd;
    while (true) {
        int saved = 0;
        const ssize_t n = conn->in.readFd(fd, &saved);
        if (n > 0) {
            conn->last_active_ms.store(nowMs(), std::memory_order_relaxed);
            conn->recv_bytes += static_cast<uint64_t>(n);
            recv_bytes_.fetch_add(static_cast<uint64_t>(n));
            if (on_message_) on_message_(fd, conn->in.peek(), static_cast<size_t>(n));
            conn->in.retrieve(static_cast<size_t>(n));
            continue;   // ET：读到 EAGAIN 为止
        }
        if (n == 0) {
            closeConnInLoop(conn, "对端正常关闭");
            return;
        }
        if (saved == EAGAIN || saved == EWOULDBLOCK) return;
        if (saved == EINTR) continue;
        LOG_WARN("fd=%d 读失败：%s", fd, std::strerror(saved));
        closeConnInLoop(conn, "读错误");
        return;
    }
}

void TcpServer::closeConn(const std::shared_ptr<Conn>& conn, const char* reason) {
    EventLoop* io = conn->loop;
    if (!io->isInLoopThread()) {
        // 跨线程关闭：派发到该连接的 owner loop，避免在别人线程里动它的 Channel
        const std::string r(reason);
        io->runInLoop([this, conn, r] { closeConnInLoop(conn, r.c_str()); });
        return;
    }
    closeConnInLoop(conn, reason);
}

void TcpServer::closeConnInLoop(const std::shared_ptr<Conn>& conn, const char* reason) {
    const int fd = conn->fd;
    {
        std::lock_guard<std::mutex> lk(conns_mtx_);
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;   // 已经关过了
        conns_.erase(it);
    }
    conn->channel.disableAll();   // 先从 poller 摘掉，再关 fd
    ::close(fd);
    LOG_INFO("关闭连接 fd=%d（%s，在线 %zu）", fd, reason, aliveConnections());
    if (on_connection_) on_connection_(fd, false);
}

void TcpServer::sweepIdle() {
    if (idle_timeout_s_ <= 0) return;
    const int64_t now = nowMs();
    const int64_t limit = static_cast<int64_t>(idle_timeout_s_) * 1000;

    std::vector<std::shared_ptr<Conn>> expired;
    {
        std::lock_guard<std::mutex> lk(conns_mtx_);
        for (auto& kv : conns_) {
            if (now - kv.second->last_active_ms.load(std::memory_order_relaxed) >= limit) {
                expired.push_back(kv.second);
            }
        }
    }
    for (auto& c : expired) {
        closed_idle_.fetch_add(1);
        closeConn(c, "空闲超时");   // 内部会派发到对应的 IO 线程
    }
    if (!expired.empty()) {
        LOG_INFO("本轮清理空闲连接 %zu 条（累计 %llu 条）", expired.size(),
                 static_cast<unsigned long long>(closed_idle_.load()));
    }
}

void TcpServer::reportStats() {
    LOG_INFO("统计：在线 %zu，累计连接 %llu，累计接收 %.2f MB，空闲清理 %llu", aliveConnections(),
             static_cast<unsigned long long>(conn_count_.load()),
             static_cast<double>(recv_bytes_.load()) / 1024.0 / 1024.0,
             static_cast<unsigned long long>(closed_idle_.load()));
}

size_t TcpServer::aliveConnections() const {
    std::lock_guard<std::mutex> lk(conns_mtx_);
    return conns_.size();
}

}  // namespace mininet
