#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "mini_net/Channel.h"
#include "mini_net/EventLoop.h"
#include "mini_net/EventLoopThreadPool.h"
#include "mini_net/MemoryPool.h"

namespace mininet {

// 门卫 + 花名册。M3：多线程版 —— base loop 只负责 accept，
// 每条连接绑定到一个 sub loop（one loop per thread），读写天然串行、几乎不需要锁。
class TcpServer {
public:
    using MessageCallback = std::function<void(int conn_fd, const char* data, size_t len)>;
    using ConnectionCallback = std::function<void(int conn_fd, bool connected)>;

    TcpServer(EventLoop* loop, uint16_t port);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;

    void setMessageCallback(MessageCallback cb) { on_message_ = std::move(cb); }
    void setConnectionCallback(ConnectionCallback cb) { on_connection_ = std::move(cb); }
    void setIdleTimeoutSeconds(int seconds) { idle_timeout_s_ = seconds; }
    void setThreadNum(int n) { thread_num_ = n; }

    void start();

    uint64_t totalConnections() const { return conn_count_.load(); }
    size_t aliveConnections() const;
    uint64_t totalReceivedBytes() const { return recv_bytes_.load(); }
    uint64_t closedIdle() const { return closed_idle_.load(); }

private:
    struct Conn;   // fd + Channel + 读缓冲 + 最后活跃时间 + 归属 loop

    void handleAccept();                                        // base loop
    void addConnection(int cfd, EventLoop* io_loop, const char* ip, uint16_t peer_port);  // io loop
    void handleReadable(const std::shared_ptr<Conn>& conn);     // io loop
    void closeConn(const std::shared_ptr<Conn>& conn, const char* reason);                // 任意线程，内部派发
    void closeConnInLoop(const std::shared_ptr<Conn>& conn, const char* reason);          // 必须 io loop
    void sweepIdle();                                           // base loop
    void reportStats();                                         // base loop

    EventLoop* loop_;
    uint16_t port_;
    int listen_fd_{-1};
    std::unique_ptr<Channel> accept_channel_;
    std::unique_ptr<EventLoopThreadPool> pool_;
    std::unique_ptr<MemoryPool> conn_pool_;   // Conn 对象池（必须在 conns_ 之前声明，保证析构顺序）
    int thread_num_{4};

    mutable std::mutex conns_mtx_;                              // 保护 conns_
    std::unordered_map<int, std::shared_ptr<Conn>> conns_;

    MessageCallback on_message_;
    ConnectionCallback on_connection_;
    std::atomic<uint64_t> conn_count_{0};
    std::atomic<uint64_t> recv_bytes_{0};
    std::atomic<uint64_t> closed_idle_{0};
    int idle_timeout_s_{30};
    int idle_timer_id_{-1};
    int stats_timer_id_{-1};
};

}  // namespace mininet
