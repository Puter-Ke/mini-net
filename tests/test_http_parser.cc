#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mini_net/Buffer.h"
#include "mini_net/HttpParser.h"

using namespace mininet;

namespace {
HttpParser::Result feed(HttpParser& p, Buffer& b, HttpParser::Error& err, const std::string& data) {
    b.append(data);
    return p.parse(&b, &err);
}
}  // namespace

TEST(HttpParserTest, ParsesSimpleGet) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    ASSERT_EQ(feed(p, b, err, "GET /abc?a=1 HTTP/1.1\r\nHost: x\r\n\r\n"), HttpParser::Result::Complete);
    const HttpRequest& req = p.request();
    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.path, "/abc");
    EXPECT_EQ(req.query, "a=1");
    EXPECT_EQ(req.target, "/abc?a=1");
    EXPECT_TRUE(req.keep_alive);
    const std::string* host = req.findHeader("host");
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(*host, "x");
}

TEST(HttpParserTest, NeedsMoreOnPartialRequest) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    EXPECT_EQ(feed(p, b, err, "GET /a HTTP/1.1\r\nHost: "), HttpParser::Result::NeedMore);
    EXPECT_EQ(feed(p, b, err, "x\r\n\r\n"), HttpParser::Result::Complete);
    EXPECT_EQ(p.request().path, "/a");
}

// ===== 回归测试：请求被拆成多段时，早先解析出的字段必须保留 =====
// 这是本项目实测踩到的 bug：外部每次 parse 都新建 HttpRequest，
// 导致请求行（method/target/version）留在被丢弃的对象里，最终路由到 404。
TEST(HttpParserTest, FieldsSurviveAcrossIncrementalFeeds) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    const std::string raw =
        "POST /api/shorten HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\n"
        "Content-Length: 18\r\nConnection: close\r\n\r\n{\"url\":\"https://a\"}";
    const size_t third = raw.size() / 3;

    EXPECT_EQ(feed(p, b, err, raw.substr(0, third)), HttpParser::Result::NeedMore);
    EXPECT_EQ(feed(p, b, err, raw.substr(third, third)), HttpParser::Result::NeedMore);
    ASSERT_EQ(feed(p, b, err, raw.substr(2 * third)), HttpParser::Result::Complete);

    const HttpRequest& req = p.request();
    EXPECT_EQ(req.method, "POST") << "请求行字段在增量解析中丢失了（这正是 404 bug 的根因）";
    EXPECT_EQ(req.path, "/api/shorten");
    EXPECT_EQ(req.version, "HTTP/1.1");
    EXPECT_EQ(req.body, "{\"url\":\"https://a\"}");
    EXPECT_FALSE(req.keep_alive);
}

// 一个字节一个字节地喂，也必须正确
TEST(HttpParserTest, FieldsSurviveByteByByteFeed) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    const std::string raw =
        "POST /api/shorten HTTP/1.1\r\nHost: t\r\nContent-Length: 7\r\n\r\n{\"u\":1}";
    HttpParser::Result last = HttpParser::Result::NeedMore;
    for (char c : raw) {
        last = feed(p, b, err, std::string(1, c));
    }
    ASSERT_EQ(last, HttpParser::Result::Complete);
    EXPECT_EQ(p.request().method, "POST");
    EXPECT_EQ(p.request().path, "/api/shorten");
    EXPECT_EQ(p.request().body, "{\"u\":1}");
}

TEST(HttpParserTest, ResetClearsRequestForNextOne) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    ASSERT_EQ(feed(p, b, err, "GET /first HTTP/1.1\r\n\r\n"), HttpParser::Result::Complete);
    EXPECT_EQ(p.request().path, "/first");

    p.reset();
    ASSERT_EQ(feed(p, b, err, "POST /second HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi"),
              HttpParser::Result::Complete);
    EXPECT_EQ(p.request().method, "POST");
    EXPECT_EQ(p.request().path, "/second");
    EXPECT_EQ(p.request().body, "hi");
    EXPECT_EQ(p.request().query, "");   // 上一个请求的字段不能残留
}

TEST(HttpParserTest, HandlesPipelinedRequests) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    ASSERT_EQ(feed(p, b, err, "GET /1 HTTP/1.1\r\n\r\nGET /2 HTTP/1.1\r\n\r\n"),
              HttpParser::Result::Complete);
    EXPECT_EQ(p.request().path, "/1");
    p.reset();
    ASSERT_EQ(p.parse(&b, &err), HttpParser::Result::Complete);
    EXPECT_EQ(p.request().path, "/2");
}

TEST(HttpParserTest, ParsesPostBody) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    const std::string raw =
        "POST /api/shorten HTTP/1.1\r\nContent-Length: 7\r\nContent-Type: application/json\r\n\r\n{\"a\":1}";
    ASSERT_EQ(feed(p, b, err, raw), HttpParser::Result::Complete);
    EXPECT_EQ(p.request().body, "{\"a\":1}");
    EXPECT_EQ(p.request().method, "POST");
}

TEST(HttpParserTest, BodySplitAcrossPackets) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    ASSERT_EQ(feed(p, b, err, "POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\n123"),
              HttpParser::Result::NeedMore);
    EXPECT_EQ(feed(p, b, err, "4567890"), HttpParser::Result::Complete);
    EXPECT_EQ(p.request().body, "1234567890");
}

TEST(HttpParserTest, RejectsGarbageRequestLine) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    EXPECT_EQ(feed(p, b, err, "!@#$%^&\r\n\r\n"), HttpParser::Result::Error);
    EXPECT_EQ(err.status, 400);
}

TEST(HttpParserTest, RejectsUnsupportedVersion) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    EXPECT_EQ(feed(p, b, err, "GET / HTTP/2.0\r\n\r\n"), HttpParser::Result::Error);
    EXPECT_EQ(err.status, 505);
}

TEST(HttpParserTest, RejectsChunkedBody) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    EXPECT_EQ(feed(p, b, err, "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"),
              HttpParser::Result::Error);
    EXPECT_EQ(err.status, 501);
}

TEST(HttpParserTest, RejectsTooLargeBody) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    EXPECT_EQ(feed(p, b, err, "POST /x HTTP/1.1\r\nContent-Length: 99999\r\n\r\n"),
              HttpParser::Result::Error);
    EXPECT_EQ(err.status, 413);
}

TEST(HttpParserTest, RejectsTooLargeHeaders) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    std::string big = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 200; ++i) big += "X-Pad-" + std::to_string(i) + ": " + std::string(50, 'p') + "\r\n";
    EXPECT_EQ(feed(p, b, err, big), HttpParser::Result::Error);
    EXPECT_EQ(err.status, 431);
}

TEST(HttpParserTest, ConnectionCloseIsHonored) {
    HttpParser p;
    Buffer b;
    HttpParser::Error err;
    ASSERT_EQ(feed(p, b, err, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n"),
              HttpParser::Result::Complete);
    EXPECT_FALSE(p.request().keep_alive);

    HttpParser p2;
    Buffer b2;
    ASSERT_EQ(feed(p2, b2, err, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"),
              HttpParser::Result::Complete);
    EXPECT_TRUE(p2.request().keep_alive);
}
