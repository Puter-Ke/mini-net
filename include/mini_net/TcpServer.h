#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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
    // 回调同时给 fd 和"连接唯一 id"。为什么需要 id？
    //   fd 会被内核复用：旧连接关闭后同一个 fd 马上可能分配给新连接，
    //   按 fd 保存"每条连接的协议解析状态"就会出现跨连接串台（本项目实测偶发 404）。
    //   id 单调递增、永不复用，因此协议层状态必须按 id 存。
    using MessageCallback = std::function<void(int conn_fd, uint64_t conn_id, const char* data, size_t len)>;
    using ConnectionCallback = std::function<void(int conn_fd, uint64_t conn_id, bool connected)>;

    TcpServer(EventLoop* loop, uint16_t port);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;

    void setMessageCallback(MessageCallback cb) { on_message_ = std::move(cb); }
    void setConnectionCallback(ConnectionCallback cb) { on_connection_ = std::move(cb); }
    void setIdleTimeoutSeconds(int seconds) { idle_timeout_s_ = seconds; }
    void setThreadNum(int n) { thread_num_ = n; }

    void start();

    // 线程安全的发送接口：可在任意线程调用（内部派发到该连接的 IO 线程）。
    // 非阻塞 fd 上 send 可能只写一部分，剩余数据进写缓冲并关注 EPOLLOUT。
    void send(int fd, const std::string& data);
    void shutdown(int fd);

    uint64_t totalConnections() const { return conn_count_.load(); }
    size_t aliveConnections() const;
    uint64_t totalReceivedBytes() const { return recv_bytes_.load(); }
    uint64_t closedIdle() const { return closed_idle_.load(); }

private:
    struct Conn;   // fd + Channel + 读缓冲 + 最后活跃时间 + 归属 loop

    void handleAccept();                                        // base loop
    void addConnection(int cfd, uint64_t conn_id, EventLoop* io_loop, const char* ip,
                       uint16_t peer_port);                                               // io loop
    void handleReadable(const std::shared_ptr<Conn>& conn);     // io loop
    void closeConn(const std::shared_ptr<Conn>& conn, const char* reason);                // 任意线程，内部派发
    void closeConnInLoop(const std::shared_ptr<Conn>& conn, const char* reason);          // 必须 io loop
    void appendAndFlush(const std::shared_ptr<Conn>& conn, const std::string& data);      // io loop
    void flushWrite(const std::shared_ptr<Conn>& conn);                                   // io loop
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
    std::atomic<uint64_t> next_conn_id_{1};   // 连接唯一 id（永不复用）
    std::atomic<uint64_t> recv_bytes_{0};
    std::atomic<uint64_t> closed_idle_{0};
    int idle_timeout_s_{30};
    int idle_timer_id_{-1};
    int stats_timer_id_{-1};
};

}  // namespace mininet
