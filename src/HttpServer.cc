#include "mini_net/HttpServer.h"

#include <cstdio>

#include "mini_net/Logger.h"

namespace mininet {

HttpServer::HttpServer(EventLoop* loop, uint16_t port) : loop_(loop), port_(port) {}

HttpServer::~HttpServer() = default;

HttpServer::HttpConn* HttpServer::findOrCreate(int fd) {
    Shard& shard = shards_[static_cast<size_t>(fd) % kShards];
    std::lock_guard<std::mutex> lk(shard.mtx);
    auto& slot = shard.conns[fd];
    if (!slot) slot = std::make_shared<HttpConn>();
    return slot.get();
}

void HttpServer::drop(int fd) {
    Shard& shard = shards_[static_cast<size_t>(fd) % kShards];
    std::lock_guard<std::mutex> lk(shard.mtx);
    shard.conns.erase(fd);
}

void HttpServer::start() {
    server_ = std::make_unique<TcpServer>(loop_, port_);
    server_->setThreadNum(thread_num_);
    server_->setIdleTimeoutSeconds(idle_timeout_s_);
    server_->setMessageCallback([this](int fd, const char* data, size_t len) {
        onMessage(fd, data, len);
    });
    server_->setConnectionCallback([this](int fd, bool connected) {
        onConnection(fd, connected);
    });
    server_->start();
    LOG_INFO("HTTP 服务已就绪（端口 %u，IO 线程 %d，空闲超时 %d 秒）", port_, thread_num_,
             idle_timeout_s_);
}

void HttpServer::onConnection(int fd, bool connected) {
    (void)connected;
    // 无论新连接还是断开，都清掉这个 fd 的解析状态。
    // 原因（实测踩到的真 bug）：内核会复用 fd。旧连接关闭后如果状态没清，
    // 新连接复用同一个 fd 时，上一条连接的"半截请求"状态会让新数据被误解析，
    // 表现为偶发 404（把请求体当成了新的请求行）。
    // 新连接的 onConnection(fd, true) 一定发生在该 fd 任何数据回调之前，所以这里清是安全的。
    drop(fd);
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

void HttpServer::onMessage(int fd, const char* data, size_t len) {
    HttpConn* conn = findOrCreate(fd);
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
