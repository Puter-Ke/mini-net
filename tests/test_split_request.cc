#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "mini_net/EventLoop.h"
#include "mini_net/HttpServer.h"
#include "mini_net/Logger.h"
#include "mini_net/ShortUrlApp.h"

using namespace mininet;

// 这一组测试专门锁死一个真实 bug：
// 连接状态按 fd 保存，而 fd 会被内核复用；如果旧连接的半截请求状态没清掉，
// 新连接的数据就会被误解析（表现为偶发 404——请求体被当成新的请求行）。
// 修复：HttpServer 在每次"连接建立/断开"时都清空该 fd 的状态。

namespace {

constexpr uint16_t kPort = 19081;

int dialWithTimeout() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
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

std::string readResponse(int fd) {
    std::string resp;
    char buf[4096];
    size_t header_end = std::string::npos;
    size_t content_length = 0;
    for (int i = 0; i < 50; ++i) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (header_end == std::string::npos) {
            header_end = resp.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const std::string hdr = resp.substr(0, header_end);
                const size_t p = hdr.find("Content-Length:");
                if (p != std::string::npos) content_length = static_cast<size_t>(std::stoul(hdr.substr(p + 15)));
            }
        }
        if (header_end != std::string::npos && resp.size() >= header_end + 4 + content_length) break;
    }
    return resp;
}

int statusOf(const std::string& r) { return r.size() < 12 ? -1 : std::stoi(r.substr(9, 3)); }

std::string shortenRequest(const std::string& url, bool keep_alive) {
    const std::string json = "{\"url\":\"" + url + "\"}";
    std::string req = "POST /api/shorten HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(json.size()) + "\r\n";
    req += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    req += "\r\n";
    req += json;
    return req;
}

}  // namespace

class SplitRequestTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        loop_ = new EventLoop();
        server_ = new HttpServer(loop_, kPort);
        app_ = new ShortUrlApp("127.0.0.1:19081");
        server_->setHandler([](const HttpRequest& q, HttpResponse* s) { app_->handle(q, s); });
        server_->setThreadNum(4);   // 故意多线程：连接会落在不同 IO 线程
        server_->setIdleTimeoutSeconds(0);
        setLogLevel(LogLevel::Debug);   // 出问题时能看到服务端把请求解析成了什么
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

EventLoop* SplitRequestTest::loop_ = nullptr;
HttpServer* SplitRequestTest::server_ = nullptr;
ShortUrlApp* SplitRequestTest::app_ = nullptr;
std::thread* SplitRequestTest::thread_ = nullptr;

// 一条请求被切成 3 段、段间有间隔地发出（真实网络就是这样），必须正确应答
TEST_F(SplitRequestTest, RequestSplitIntoThreeChunks) {
    for (int round = 0; round < 20; ++round) {
        const int fd = dialWithTimeout();
        ASSERT_GE(fd, 0);
        const std::string req = shortenRequest("https://example.com/split/" + std::to_string(round), false);
        const size_t third = req.size() / 3;
        const std::vector<std::string> chunks = {req.substr(0, third), req.substr(third, third),
                                                 req.substr(2 * third)};
        for (const auto& c : chunks) {
            ASSERT_EQ(::send(fd, c.data(), c.size(), 0), static_cast<ssize_t>(c.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const std::string resp = readResponse(fd);
        ::close(fd);
        if (statusOf(resp) != 201) {
            std::printf("SPLIT-DEBUG 第 %d 轮 status=%d 原始响应=[%s]\n", round, statusOf(resp),
                        resp.substr(0, 200).c_str());
        }
        EXPECT_EQ(statusOf(resp), 201) << "第 " << round << " 轮拆包失败，响应："
                                       << resp.substr(0, 120);
    }
}

// 3 条完整请求一次发出（粘包），三条都要被正确应答
TEST_F(SplitRequestTest, ThreeRequestsCoalescedInOnePacket) {
    const int fd = dialWithTimeout();
    ASSERT_GE(fd, 0);
    std::string all;
    for (int i = 0; i < 3; ++i) {
        all += shortenRequest("https://example.com/coalesced/" + std::to_string(i), true);
    }
    ASSERT_EQ(::send(fd, all.data(), all.size(), 0), static_cast<ssize_t>(all.size()));
    for (int i = 0; i < 3; ++i) {
        const std::string resp = readResponse(fd);
        EXPECT_EQ(statusOf(resp), 201) << "第 " << i + 1 << " 条粘包请求异常：" << resp.substr(0, 120);
    }
    ::close(fd);
}

// 关键回归：连续快速建立/关闭连接（触发 fd 复用），每条请求都必须拿到自己的应答
TEST_F(SplitRequestTest, RapidConnectCloseDoesNotLeakParserState) {
    for (int round = 0; round < 40; ++round) {
        const int fd = dialWithTimeout();
        ASSERT_GE(fd, 0);
        const std::string req = shortenRequest("https://example.com/rapid/" + std::to_string(round), false);
        ASSERT_EQ(::send(fd, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));
        const std::string resp = readResponse(fd);
        ::close(fd);
        ASSERT_EQ(statusOf(resp), 201) << "第 " << round << " 轮失败（fd 复用时读到了旧解析状态）";
        ASSERT_NE(resp.find("https://example.com/rapid/" + std::to_string(round)), std::string::npos)
            << "第 " << round << " 轮回显的 url 不是本次请求的（响应串台）";
    }
}

// 原样复现 pytest 里报 404 的场景：20 条连接并发创建同一个 URL
TEST_F(SplitRequestTest, ConcurrentSameUrlIsIdempotent) {
    constexpr int kThreads = 20;
    const std::string url = "https://example.com/concurrent/same-url";
    std::vector<std::thread> workers;
    std::vector<int> statuses(kThreads, -1);
    std::vector<std::string> codes(kThreads);
    std::vector<std::string> raw(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&, i] {
            const int fd = dialWithTimeout();
            if (fd < 0) return;
            const std::string req = shortenRequest(url, true);
            if (::send(fd, req.data(), req.size(), 0) != static_cast<ssize_t>(req.size())) {
                ::close(fd);
                return;
            }
            const std::string resp = readResponse(fd);
            ::close(fd);
            raw[static_cast<size_t>(i)] = resp.substr(0, 100);
            statuses[static_cast<size_t>(i)] = statusOf(resp);
            const size_t p = resp.find("\"code\":\"");
            if (p != std::string::npos) {
                const size_t begin = p + 8;
                codes[static_cast<size_t>(i)] = resp.substr(begin, resp.find('"', begin) - begin);
            }
        });
    }
    for (auto& w : workers) w.join();

    for (int i = 0; i < kThreads; ++i) {
        EXPECT_EQ(statuses[static_cast<size_t>(i)], 201)
            << "第 " << i << " 条并发请求状态码异常：" << raw[static_cast<size_t>(i)];
    }
    const std::string first = codes[0];
    EXPECT_FALSE(first.empty());
    for (int i = 1; i < kThreads; ++i) {
        EXPECT_EQ(codes[static_cast<size_t>(i)], first) << "同一 URL 并发创建出现了多个短码";
    }
}

// 精确复现 pytest 里报 404 的调用方式：**同一条 keep-alive 连接**上的多次顺序请求，
// 每次都要先读完上一条的响应再发下一条（requests 的 Session 就是这么用的）
TEST_F(SplitRequestTest, SequentialRequestsOnKeepAliveConnection) {
    const int fd = dialWithTimeout();
    ASSERT_GE(fd, 0);
    for (int i = 0; i < 5; ++i) {
        const std::string url = "https://example.com/seq/" + std::to_string(i);
        const std::string req = shortenRequest(url, true);
        ASSERT_EQ(::send(fd, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));
        const std::string resp = readResponse(fd);
        ASSERT_EQ(statusOf(resp), 201) << "第 " << i + 1 << " 次顺序请求失败：" << resp.substr(0, 150);
        EXPECT_NE(resp.find(url), std::string::npos) << "第 " << i + 1 << " 次响应串台：" << resp.substr(0, 150);
    }
    ::close(fd);
}

// 模拟一个 Session 的混合调用：创建 → 跳转 → 统计 → 再创建，全在一条连接上
TEST_F(SplitRequestTest, MixedOperationsOnOneKeepAliveConnection) {
    const int fd = dialWithTimeout();
    ASSERT_GE(fd, 0);

    const std::string json = "{\"url\":\"https://example.com/mixed\"}";
    std::string req = "POST /api/shorten HTTP/1.1\r\nHost: t\r\nContent-Length: " +
                      std::to_string(json.size()) + "\r\nConnection: keep-alive\r\n\r\n" + json;
    ASSERT_EQ(::send(fd, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));
    const std::string r1 = readResponse(fd);
    ASSERT_EQ(statusOf(r1), 201) << r1.substr(0, 150);
    const size_t p = r1.find("\"code\":\"");
    ASSERT_NE(p, std::string::npos);
    const size_t begin = p + 8;
    const std::string code = r1.substr(begin, r1.find('"', begin) - begin);
    ASSERT_FALSE(code.empty());

    // 跳转
    const std::string r2req = "GET /" + code + " HTTP/1.1\r\nHost: t\r\nConnection: keep-alive\r\n\r\n";
    ASSERT_EQ(::send(fd, r2req.data(), r2req.size(), 0), static_cast<ssize_t>(r2req.size()));
    const std::string r2 = readResponse(fd);
    EXPECT_EQ(statusOf(r2), 302) << r2.substr(0, 150);

    // 统计
    const std::string r3req =
        "GET /api/stats/" + code + " HTTP/1.1\r\nHost: t\r\nConnection: keep-alive\r\n\r\n";
    ASSERT_EQ(::send(fd, r3req.data(), r3req.size(), 0), static_cast<ssize_t>(r3req.size()));
    const std::string r3 = readResponse(fd);
    EXPECT_EQ(statusOf(r3), 200) << r3.substr(0, 150);
    EXPECT_NE(r3.find("\"hits\":1"), std::string::npos) << r3.substr(0, 150);

    // 再创建一条，验证连接还能继续用
    const std::string json2 = "{\"url\":\"https://example.com/mixed-2\"}";
    std::string req2 = "POST /api/shorten HTTP/1.1\r\nHost: t\r\nContent-Length: " +
                       std::to_string(json2.size()) + "\r\nConnection: keep-alive\r\n\r\n" + json2;
    ASSERT_EQ(::send(fd, req2.data(), req2.size(), 0), static_cast<ssize_t>(req2.size()));
    const std::string r4 = readResponse(fd);
    EXPECT_EQ(statusOf(r4), 201) << r4.substr(0, 150);

    ::close(fd);
}

// 中文 URL 的 JSON 转义必须被正确还原（\u4e2d 这类）
TEST_F(SplitRequestTest, UnicodeEscapedUrlIsDecoded) {
    const int fd = dialWithTimeout();
    ASSERT_GE(fd, 0);
    const std::string url = "https://example.com/\u4e2d\u6587?k=\u503c";
    // 手工构造 \uXXXX 转义（模拟部分客户端的行为）
    std::string json = "{\"url\":\"https://example.com/\\u4e2d\\u6587?k=\\u503c\"}";
    std::string req = "POST /api/shorten HTTP/1.1\r\nHost: t\r\nContent-Length: " +
                      std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
    ASSERT_EQ(::send(fd, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));
    const std::string resp = readResponse(fd);
    ::close(fd);
    ASSERT_EQ(statusOf(resp), 201) << resp.substr(0, 150);
    EXPECT_NE(resp.find(url), std::string::npos) << "转义没有被还原：" << resp.substr(0, 200);
}

// 被截断的 JSON 必须报 400，不能被当成合法请求接受
TEST_F(SplitRequestTest, TruncatedJsonIsRejected) {
    const int fd = dialWithTimeout();
    ASSERT_GE(fd, 0);
    const std::string json = "{\"url\":\"https://example.com/truncated\"";
    std::string req = "POST /api/shorten HTTP/1.1\r\nHost: t\r\nContent-Length: " +
                      std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
    ASSERT_EQ(::send(fd, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));
    const std::string resp = readResponse(fd);
    ::close(fd);
    EXPECT_EQ(statusOf(resp), 400) << "截断的 JSON 被接受了：" << resp.substr(0, 150);
}
