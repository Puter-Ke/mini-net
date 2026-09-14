#include "mini_net/HttpServer.h"

#include <cstdio>

#include "mini_net/Logger.h"

namespace mininet {

HttpServer::HttpServer(EventLoop* loop, uint16_t port) : loop_(loop), port_(port) {}

HttpServer::~HttpServer() = default;

HttpServer::HttpConn* HttpServer::findOrCreate(uint64_t conn_id) {
    Shard& shard = shards_[shardOf(conn_id)];
    std::lock_guard<std::mutex> lk(shard.mtx);
    auto& slot = shard.conns[conn_id];
    if (!slot) slot = std::make_shared<HttpConn>();
    return slot.get();
}

void HttpServer::drop(uint64_t conn_id) {
    Shard& shard = shards_[shardOf(conn_id)];
    std::lock_guard<std::mutex> lk(shard.mtx);
    shard.conns.erase(conn_id);
}

void HttpServer::start() {
    server_ = std::make_unique<TcpServer>(loop_, port_);
    server_->setThreadNum(thread_num_);
    server_->setIdleTimeoutSeconds(idle_timeout_s_);
    server_->setMessageCallback([this](int fd, uint64_t conn_id, const char* data, size_t len) {
        onMessage(fd, conn_id, data, len);
    });
    server_->setConnectionCallback([this](int fd, uint64_t conn_id, bool connected) {
        onConnection(fd, conn_id, connected);
    });
    server_->start();
    LOG_INFO("HTTP 服务已就绪（端口 %u，IO 线程 %d，空闲超时 %d 秒）", port_, thread_num_,
             idle_timeout_s_);
}

void HttpServer::onConnection(int fd, uint64_t conn_id, bool connected) {
    (void)fd;
    // 连接建立时清一次（保证新连接从零状态开始），断开时释放内存。
    // 注意这里是按 conn_id 清，而不是按 fd —— fd 会被复用，按 fd 清会误删新连接的状态
    // （旧连接关闭的清理动作可能晚于新连接的状态创建，实测表现为偶发 404）。
    drop(conn_id);
}

void HttpServer::handleError(int fd, const HttpParser::Error& err) {
    errors_.fetch_add(1);
    HttpResponse resp;
    resp.status = err.status;
    resp.keep_alive = false;   // 协议错误一律关闭连接
    resp.setJson("{\"error\":\"" + err.code + "\",\"detail\":\"" + err.detail + "\"}");
    server_->send(fd, resp.serialize());
    server_->shutdown(fd);
}

void HttpServer::onMessage(int fd, uint64_t conn_id, const char* data, size_t len) {
    HttpConn* conn = findOrCreate(conn_id);
    conn->in.append(data, len);

    while (true) {
        HttpRequest req;
        HttpParser::Error err;
        const HttpParser::Result r = conn->parser.parse(&conn->in, &req, &err);

        if (r == HttpParser::Result::NeedMore) return;
        if (r == HttpParser::Result::Error) {
            handleError(fd, err);
            return;
        }

        requests_.fetch_add(1);
        HttpResponse resp;
        resp.keep_alive = req.keep_alive;
        if (handler_) {
            handler_(req, &resp);
        } else {
            resp.status = 404;
            resp.setJson("{\"error\":\"not_found\",\"detail\":\"没有设置 handler\"}");
        }

        // 响应发完再关（小响应一次 write 就出去了；大数据量需要"写完再关"的机制）
        server_->send(fd, resp.serialize());
        if (!req.keep_alive || !resp.keep_alive) {
            server_->shutdown(fd);
            return;
        }
        conn->parser.reset();
    }
}

std::string HttpServer::buildMetrics() const {
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - started_at_)
                            .count();
    std::string out;
    char line[256];
    std::snprintf(line, sizeof line, "uptime_seconds %lld\n", static_cast<long long>(uptime));
    out += line;
    if (server_) {
        std::snprintf(line, sizeof line, "alive_connections %zu\n", server_->aliveConnections());
        out += line;
        std::snprintf(line, sizeof line, "total_connections %llu\n",
                      static_cast<unsigned long long>(server_->totalConnections()));
        out += line;
        std::snprintf(line, sizeof line, "total_bytes_in %llu\n",
                      static_cast<unsigned long long>(server_->totalReceivedBytes()));
        out += line;
    }
    std::snprintf(line, sizeof line, "total_requests %llu\n",
                  static_cast<unsigned long long>(requests_.load()));
    out += line;
    if (metrics_provider_) metrics_provider_(out);
    return out;
}

}  // namespace mininet
