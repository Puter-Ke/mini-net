#include <string>

#include <gtest/gtest.h>

#include "mini_net/Buffer.h"
#include "mini_net/HttpParser.h"

using namespace mininet;

namespace {
HttpParser::Result feed(HttpParser& p, Buffer& b, HttpRequest& req, HttpParser::Error& err,
                        const std::string& data) {
    b.append(data);
    return p.parse(&b, &req, &err);
}
}  // namespace

TEST(HttpParserTest, ParsesSimpleGet) {
    HttpParser p;
    Buffer b;
    HttpRequest req;
    HttpParser::Error err;
    ASSERT_EQ(feed(p, b, req, err, "GET /abc?a=1 HTTP/1.1\r\nHost: x\r\n\r\n"), HttpParser::Result::Complete);
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
    HttpRequest req;
    HttpParser::Error err;
    EXPECT_EQ(feed(p, b, req, err, "GET /a HTTP/1.1\r\nHost: "), HttpParser::Result::NeedMore);
    EXPECT_EQ(feed(p, b, req, err, "x\r\n\r\n"), HttpParser::Result::Complete);
    EXPECT_EQ(req.path, "/a");
}

TEST(HttpParserTest, HandlesPipelinedRequests) {
    // 两个完整请求一次到达（粘包）：解析器一次只吃一个，剩下的留在缓冲里
    HttpParser p;
    Buffer b;
    HttpRequest req;
    HttpParser::Error err;
    ASSERT_EQ(feed(p, b, req, err, "GET /1 HTTP/1.1\r\n\r\nGET /2 HTTP/1.1\r\n\r\n"),
              HttpParser::Result::Complete);
    EXPECT_EQ(req.path, "/1");
    p.reset();
    req = HttpRequest{};
    ASSERT_EQ(p.parse(&b, &req, &err), HttpParser::Result::Complete);
    EXPECT_EQ(req.path, "/2");
}

TEST(HttpParserTest, ParsesPostBody) {
    HttpParser p;
    Buffer b;
    HttpRequest req;
    HttpParser::Error err;
    const std::string raw =
        "POST /api/shorten HTTP/1.1\r\nContent-Length: 7\r\nContent-Type: application/json\r\n\r\n{\"a\":1}";
    ASSERT_EQ(feed(p, b, req, err, raw), HttpParser::Result::Complete);
    EXPECT_EQ(req.body, "{\"a\":1}");
    EXPECT_EQ(req.method, "POST");
}

TEST(HttpParserTest, BodySplitAcrossPackets) {
    HttpParser p;
    Buffer b;
    HttpRequest req;
    HttpParser::Error err;
    ASSERT_EQ(feed(p, b, req, err,
                   "POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\n123"),
              HttpParser::Result::NeedMore);
    EXPECT_EQ(feed(p, b, req, err, "4567890"), HttpParser::Result::Complete);
    EXPECT_EQ(req.body, "1234567890");
}

TEST(HttpParserTest, RejectsGarbageRequestLine) {
    HttpParser p;
    Buffer b;
    HttpRequest req;
    HttpParser::Error err;
    EXPECT_EQ(feed(p, b, req, err, "!@#$%^&\r\n\r\n"), HttpParser::Result::Error);
    EXPECT_EQ(err.status, 400);
}

TEST(HttpParserTest, RejectsUnsupportedVersion) {
    HttpParser p;
    Buffer b;
    HttpRequest req;
    HttpParser::Error err;
    EXPECT_EQ(feed(p, b, req, err, "GET / HTTP/2.0\r\n\r\n"), HttpParser::Result::Error);
    EXPECT_EQ(err.status, 505);
}

TEST(HttpParserTest, RejectsChunkedBody) {
    HttpParser p;
    Buffer b;
    HttpRequest req;
    HttpParser::Error err;
    EXPECT_EQ(feed(p, b, req, err, "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"),
              HttpParser::Result::Error);
    EXPECT_EQ(err.status, 501);
}

TEST(HttpParserTest, RejectsTooLargeBody) {
    HttpParser p;
    Buffer b;
    HttpRequest req;
    HttpParser::Error err;
    EXPECT_EQ(feed(p, b, req, err, "POST /x HTTP/1.1\r\nContent-Length: 99999\r\n\r\n"),
              HttpParser::Result::Error);
    EXPECT_EQ(err.status, 413);
}

TEST(HttpParserTest, RejectsTooLargeHeaders) {
    HttpParser p;
    Buffer b;
    HttpRequest req;
    HttpParser::Error err;
    std::string big = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 200; ++i) big += "X-Pad-" + std::to_string(i) + ": " + std::string(50, 'p') + "\r\n";
    EXPECT_EQ(feed(p, b, req, err, big), HttpParser::Result::Error);
    EXPECT_EQ(err.status, 431);
}

TEST(HttpParserTest, ConnectionCloseIsHonored) {
    HttpParser p;
    Buffer b;
    HttpRequest req;
    HttpParser::Error err;
    ASSERT_EQ(feed(p, b, req, err, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n"),
              HttpParser::Result::Complete);
    EXPECT_FALSE(req.keep_alive);

    HttpParser p2;
    Buffer b2;
    HttpRequest req2;
    ASSERT_EQ(feed(p2, b2, req2, err, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"),
              HttpParser::Result::Complete);
    EXPECT_TRUE(req2.keep_alive);   // HTTP/1.0 显式声明 keep-alive
}
