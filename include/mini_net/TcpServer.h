#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include "mini_net/Channel.h"
#include "mini_net/EventLoop.h"

namespace mininet {

// 门卫 + 花名册：listen / accept / 管理连接生命周期
class TcpServer {
public:
    using MessageCallback = std::function<void(int conn_fd, const char* data, size_t len)>;
    using ConnectionCallback = std::function<void(int conn_fd, bool connected)>;

    TcpServer(EventLoop* loop, uint16_t port);
    ~TcpServer();

    void setMessageCallback(MessageCallback cb) { on_message_ = std::move(cb); }
    void setConnectionCallback(ConnectionCallback cb) { on_connection_ = std::move(cb); }

    void start();
    uint64_t totalConnections() const { return conn_count_; }
    size_t aliveConnections() const { return conns_.size(); }

private:
    struct Conn;   // 每条连接的私有状态（fd + Channel + 读缓冲），定义在 .cc 里

    void handleAccept();
    void handleReadable(int fd);
    void closeConn(int fd);

    EventLoop* loop_;
    uint16_t port_;
    int listen_fd_{-1};
    std::unique_ptr<Channel> accept_channel_;
    std::unordered_map<int, std::shared_ptr<Conn>> conns_;
    MessageCallback on_message_;
    ConnectionCallback on_connection_;
    uint64_t conn_count_{0};
};

}  // namespace mininet
