#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "mini_net/EventLoop.h"
#include "mini_net/HttpServer.h"
#include "mini_net/ShortUrlApp.h"

using namespace mininet;

namespace {

constexpr uint16_t kPort = 19080;

int dial() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    // 关键：给客户端加接收超时。否则服务端一旦不回响应，recv 会永久阻塞，
    // ctest 只能靠整体超时杀掉进程，报出来的是"Timeout"而不是真实原因。
    timeval tv{};
    tv.tv_sec = 3;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// 发一个请求并把完整响应读回来（按 Content-Length 判断读完）
std::string roundTrip(int fd, const std::string& request) {
    if (::send(fd, request.data(), request.size(), 0) < 0) return "";
    std::string resp;
    char buf[4096];
    size_t header_end = std::string::npos;
    size_t content_length = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (header_end == std::string::npos) {
            header_end = resp.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const std::string lower_hdr = resp.substr(0, header_end);
                const size_t pos = lower_hdr.find("Content-Length:");
                if (pos != std::string::npos) {
                    content_length = static_cast<size_t>(std::stoul(lower_hdr.substr(pos + 15)));
                }
            }
        }
        if (header_end != std::string::npos && resp.size() >= header_end + 4 + content_length) break;
    }
    return resp;
}

int statusOf(const std::string& resp) {
    if (resp.size() < 12) return -1;
    return std::stoi(resp.substr(9, 3));
}

std::string bodyOf(const std::string& resp) {
    const size_t p = resp.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : resp.substr(p + 4);
}

}  // namespace

class HttpApiTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        loop_ = new EventLoop();
        server_ = new HttpServer(loop_, kPort);
        app_ = new ShortUrlApp("127.0.0.1:19080");
        app_->setMetricsSource([] { return server_->buildMetrics(); });
        server_->setMetricsProvider([](std::string& out) { app_->appendMetrics(out); });
        server_->setHandler([](const HttpRequest& req, HttpResponse* resp) { app_->handle(req, resp); });
        server_->setThreadNum(2);
        server_->setIdleTimeoutSeconds(0);
        server_->start();
        thread_ = new std::thread([] { loop_->loop(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    static void TearDownTestSuite() {
        loop_->quit();
        thread_->join();
        delete server_;
        delete app_;
        delete loop_;
    }

    static EventLoop* loop_;
    static HttpServer* server_;
    static ShortUrlApp* app_;
    static std::thread* thread_;
};

EventLoop* HttpApiTest::loop_ = nullptr;
HttpServer* HttpApiTest::server_ = nullptr;
ShortUrlApp* HttpApiTest::app_ = nullptr;
std::thread* HttpApiTest::thread_ = nullptr;

TEST_F(HttpApiTest, HealthzReturnsOk) {
    const int fd = dial();
    ASSERT_GE(fd, 0);
    const std::string resp = roundTrip(fd, "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n");
    ::close(fd);
    EXPECT_EQ(statusOf(resp), 200);
    EXPECT_EQ(bodyOf(resp), "ok");
}

TEST_F(HttpApiTest, ShortenThenRedirectThenStats) {
    const int fd = dial();
    ASSERT_GE(fd, 0);

    const std::string json = "{\"url\":\"https://a.com/1\"}";
    const std::string shortener = "POST /api/shorten HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                                  std::to_string(json.size()) + "\r\n\r\n" + json;
    const std::string resp1 = roundTrip(fd, shortener);
    ASSERT_EQ(statusOf(resp1), 201) << resp1;
    const std::string body1 = bodyOf(resp1);
    const size_t code_pos = body1.find("\"code\":\"");
    ASSERT_NE(code_pos, std::string::npos);
    const std::string code = body1.substr(code_pos + 8, body1.find('"', code_pos + 8) - (code_pos + 8));
    EXPECT_FALSE(code.empty());

    // 跳转
    const std::string resp2 = roundTrip(fd, "GET /" + code + " HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_EQ(statusOf(resp2), 302);
    EXPECT_NE(resp2.find("Location: https://a.com/1"), std::string::npos);

    // 统计：hits 至少为 1
    const std::string resp3 = roundTrip(fd, "GET /api/stats/" + code + " HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_EQ(statusOf(resp3), 200);
    EXPECT_NE(bodyOf(resp3).find("\"hits\":1"), std::string::npos) << bodyOf(resp3);

    ::close(fd);
}

TEST_F(HttpApiTest, KeepAliveServesMultipleRequests) {
    const int fd = dial();
    ASSERT_GE(fd, 0);
    for (int i = 0; i < 3; ++i) {
        const std::string resp = roundTrip(fd, "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n");
        EXPECT_EQ(statusOf(resp), 200) << "第 " << i + 1 << " 个请求失败";
        EXPECT_NE(resp.find("Connection: keep-alive"), std::string::npos);
    }
    ::close(fd);
}

TEST_F(HttpApiTest, DuplicateUrlReturnsSameCode) {
    const int fd = dial();
    ASSERT_GE(fd, 0);
    const std::string json = "{\"url\":\"https://b.com/2\"}";
    const std::string req = "POST /api/shorten HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                            std::to_string(json.size()) + "\r\n\r\n" + json;
    const std::string r1 = roundTrip(fd, req);
    const std::string r2 = roundTrip(fd, req);
    ::close(fd);
    ASSERT_EQ(statusOf(r1), 201);
    ASSERT_EQ(statusOf(r2), 201);
    EXPECT_EQ(bodyOf(r1), bodyOf(r2)) << "同一 URL 应返回同一个短码";
}

TEST_F(HttpApiTest, InvalidUrlRejected) {
    const int fd = dial();
    ASSERT_GE(fd, 0);
    const std::string body = "{\"url\":\"ftp://bad\"}";
    const std::string req = "POST /api/shorten HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    const std::string resp = roundTrip(fd, req);
    ::close(fd);
    EXPECT_EQ(statusOf(resp), 400);
    EXPECT_NE(bodyOf(resp).find("invalid_url"), std::string::npos);
}

TEST_F(HttpApiTest, UnknownCodeReturns404) {
    const int fd = dial();
    ASSERT_GE(fd, 0);
    const std::string resp = roundTrip(fd, "GET /zzzzz9 HTTP/1.1\r\nHost: x\r\n\r\n");
    ::close(fd);
    EXPECT_EQ(statusOf(resp), 404);
}

TEST_F(HttpApiTest, WrongMethodReturns405WithAllow) {
    const int fd = dial();
    ASSERT_GE(fd, 0);
    const std::string resp = roundTrip(fd, "GET /api/shorten HTTP/1.1\r\nHost: x\r\n\r\n");
    ::close(fd);
    EXPECT_EQ(statusOf(resp), 405);
    EXPECT_NE(resp.find("Allow: POST"), std::string::npos);
}

TEST_F(HttpApiTest, OversizedBodyReturns413) {
    const int fd = dial();
    ASSERT_GE(fd, 0);
    const std::string req =
        "POST /api/shorten HTTP/1.1\r\nHost: x\r\nContent-Length: 99999\r\n\r\n" + std::string(100, 'x');
    const std::string resp = roundTrip(fd, req);
    ::close(fd);
    EXPECT_EQ(statusOf(resp), 413);
}

TEST_F(HttpApiTest, MetricsHasExpectedKeys) {
    const int fd = dial();
    ASSERT_GE(fd, 0);
    roundTrip(fd, "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n");
    const std::string resp = roundTrip(fd, "GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n");
    ::close(fd);
    EXPECT_EQ(statusOf(resp), 200);
    const std::string body = bodyOf(resp);
    for (const char* key : {"uptime_seconds", "alive_connections", "total_connections", "total_requests",
                            "total_bytes_in", "shorten_total", "redirect_total"}) {
        EXPECT_NE(body.find(key), std::string::npos) << "缺少指标 " << key;
    }
}
