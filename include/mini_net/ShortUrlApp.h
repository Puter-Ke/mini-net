#pragma once
#include <functional>
#include <string>

#include "mini_net/HttpServer.h"
#include "mini_net/HttpTypes.h"
#include "mini_net/ShortUrlService.h"

namespace mininet {

// 路由层：把 HTTP 请求映射到短链业务（接口契约见 docs/API.md）
class ShortUrlApp {
public:
    explicit ShortUrlApp(std::string host) : host_(std::move(host)) {}

    void handle(const HttpRequest& req, HttpResponse* resp);
    void appendMetrics(std::string& out) const;
    void setMetricsSource(std::function<std::string()> src) { metrics_source_ = std::move(src); }
    ShortUrlService& service() { return service_; }
    const ShortUrlService& service() const { return service_; }

private:
    void handleShorten(const HttpRequest& req, HttpResponse* resp);
    void handleRedirect(const std::string& code, HttpResponse* resp);
    void handleStats(const std::string& code, HttpResponse* resp);

    std::string host_;
    ShortUrlService service_;
    std::function<std::string()> metrics_source_;
};

}  // namespace mininet
