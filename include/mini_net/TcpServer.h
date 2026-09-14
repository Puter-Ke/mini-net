#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include "mini_net/Channel.h"
#include "mini_net/EventLoop.h"

namespace mininet {

// 门卫 + 花名册：listen / accept / 管理连接生命周期 / 踢掉空闲连接
class TcpServer {
public:
    using MessageCallback = std::function<void(int conn_fd, const char* data, size_t len)>;
    using ConnectionCallback = std::function<void(int conn_fd, bool connected)>;

    TcpServer(EventLoop* loop, uint16_t port);
    ~TcpServer();

    void setMessageCallback(MessageCallback cb) { on_message_ = std::move(cb); }
    void setConnectionCallback(ConnectionCallback cb) { on_connection_ = std::move(cb); }

    // 空闲超过 seconds 秒没有任何数据往来就关闭连接；<=0 表示关闭该功能
    void setIdleTimeoutSeconds(int seconds) { idle_timeout_s_ = seconds; }

    void start();
    uint64_t totalConnections() const { return conn_count_; }
    size_t aliveConnections() const { return conns_.size(); }
    uint64_t totalReceivedBytes() const { return recv_bytes_; }

private:
    struct Conn;   // 每条连接的私有状态（fd + Channel + 读缓冲 + 最后活跃时间）

    void handleAccept();
    void handleReadable(int fd);
    void closeConn(int fd, const char* reason);
    void sweepIdle();     // 周期扫描，踢掉空闲连接
    void reportStats();   // 周期打印统计

    EventLoop* loop_;
    uint16_t port_;
    int listen_fd_{-1};
    std::unique_ptr<Channel> accept_channel_;
    std::unordered_map<int, std::shared_ptr<Conn>> conns_;
    MessageCallback on_message_;
    ConnectionCallback on_connection_;
    uint64_t conn_count_{0};       // 历史累计连接数
    uint64_t recv_bytes_{0};       // 累计收到字节数
    uint64_t closed_idle_{0};      // 因空闲被踢掉的连接数
    int idle_timeout_s_{30};
    int idle_timer_id_{-1};
    int stats_timer_id_{-1};
};

}  // namespace mininet
