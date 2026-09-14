#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "mini_net/EventLoop.h"
#include "mini_net/HttpParser.h"
#include "mini_net/HttpTypes.h"
#include "mini_net/TcpServer.h"

namespace mininet {

// HTTP 服务器：在 TcpServer（传输层）之上做协议解析与路由
// 分层：TcpServer 只管字节流收发，HttpServer 只管"解析成请求 / 序列化响应"，业务在 handler 里
class HttpServer {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse*)>;
    using MetricsProvider = std::function<void(std::string& out)>;

    HttpServer(EventLoop* loop, uint16_t port);
    ~HttpServer();

    void setHandler(Handler h) { handler_ = std::move(h); }
    void setMetricsProvider(MetricsProvider p) { metrics_provider_ = std::move(p); }
    void setThreadNum(int n) { thread_num_ = n; }
    void setIdleTimeoutSeconds(int s) { idle_timeout_s_ = s; }
    void start();

    uint64_t totalRequests() const { return requests_.load(); }
    uint64_t totalErrors() const { return errors_.load(); }
    TcpServer* tcp() { return server_.get(); }

    // 供 /metrics 接口使用：服务端统计 + 业务方通过 MetricsProvider 追加的指标
    std::string buildMetrics() const;

private:
    struct HttpConn {
        Buffer in;
        HttpParser parser;
    };

    // 分片锁：fd 决定分片，不同 IO 线程通常落在不同分片，减少锁竞争
    static constexpr size_t kShards = 16;
    // 分片锁：按连接 id 分片。**不要按 fd 分片**——fd 会被复用，会造成跨连接串台
    struct Shard {
        std::mutex mtx;
        std::unordered_map<uint64_t, std::shared_ptr<HttpConn>> conns;
    };

    HttpConn* findOrCreate(uint64_t conn_id);
    void drop(uint64_t conn_id);
    void onMessage(int fd, uint64_t conn_id, const char* data, size_t len);
    void onConnection(int fd, uint64_t conn_id, bool connected);
    static size_t shardOf(uint64_t conn_id) { return static_cast<size_t>(conn_id % kShards); }
    void handleError(int fd, const HttpParser::Error& err);

    EventLoop* loop_;
    uint16_t port_;
    int thread_num_{4};
    int idle_timeout_s_{30};
    std::unique_ptr<TcpServer> server_;
    Handler handler_;
    MetricsProvider metrics_provider_;
    std::array<Shard, kShards> shards_;
    std::atomic<uint64_t> requests_{0};
    std::atomic<uint64_t> errors_{0};
    std::chrono::steady_clock::time_point started_at_{std::chrono::steady_clock::now()};
};

}  // namespace mininet
