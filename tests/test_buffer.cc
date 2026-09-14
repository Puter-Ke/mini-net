#include <fcntl.h>
#include <unistd.h>

#include <string>

#include <gtest/gtest.h>

#include "mini_net/Buffer.h"

using namespace mininet;

TEST(BufferTest, AppendAndRetrieve) {
    Buffer buf;
    buf.append("hello", 5);
    EXPECT_EQ(buf.readableBytes(), 5u);
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), "hello");
    buf.retrieve(2);
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), "llo");
    buf.retrieveAll();
    EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(BufferTest, SpaceIsReusedAfterRetrieveAll) {
    Buffer buf(64);
    const size_t capacity_before = buf.writableBytes() + buf.readableBytes();

    for (int round = 0; round < 50; ++round) {
        buf.append("0123456789", 10);
        buf.retrieveAll();                 // 每轮都取空
    }
    EXPECT_EQ(buf.writableBytes() + buf.readableBytes(), capacity_before)
        << "反复追加/取空之后底层空间被浪费了（环形复用失效）";
}

TEST(BufferTest, ReusesPrependableSpaceWithoutGrowing) {
    Buffer buf(128);
    const size_t total_capacity = buf.prependableBytes() + buf.writableBytes() + buf.readableBytes();

    // 制造"已读空洞"：写 100、取走 50（产生 50 字节空洞）、再写 40
    // 正确实现应该复用空洞（memmove 复位），而不是扩容
    buf.append(std::string(100, 'a'));
    buf.retrieve(50);
    buf.append(std::string(40, 'b'));

    EXPECT_EQ(buf.prependableBytes() + buf.writableBytes() + buf.readableBytes(), total_capacity)
        << "底层缓冲被扩容了，说明没有复用已读空间";
    EXPECT_EQ(buf.readableBytes(), 90u);   // 100 - 50 + 40
}

TEST(BufferTest, GrowsWhenNeeded) {
    Buffer buf(64);
    const std::string big(10000, 'x');
    buf.append(big);
    EXPECT_EQ(buf.readableBytes(), big.size());
    EXPECT_EQ(buf.retrieveAllAsString(), big);
}

TEST(BufferTest, FindCrlfAndRetrieveUntil) {
    Buffer buf;
    buf.append("GET /a HTTP/1.1\r\nHost: x\r\n\r\nBODY");

    const char* crlf = buf.findCRLF();
    ASSERT_NE(crlf, nullptr);
    // findCRLF 返回 \r 的位置，前面就是请求行
    EXPECT_EQ(std::string(buf.peek(), static_cast<size_t>(crlf - buf.peek())), "GET /a HTTP/1.1");

    buf.retrieveUntil(crlf + 2);   // 语义：[peek, end)，所以要 +2 才能把 \r\n 一起带走
    EXPECT_EQ(buf.retrieveAllAsString(), "Host: x\r\n\r\nBODY");
}

TEST(BufferTest, FindCrlfOnSplitInput) {
    // 真实网络里 \r 和 \n 可能分两次到达：找不到时必须返回 nullptr，不能越界读
    Buffer buf;
    buf.append("GET /a HTTP/1.1\r", 16);   // 15 个字符 + \r
    EXPECT_EQ(buf.findCRLF(), nullptr);
    buf.append("\n", 1);
    ASSERT_NE(buf.findCRLF(), nullptr);
    EXPECT_EQ(std::string(buf.peek(), static_cast<size_t>(buf.findCRLF() - buf.peek())), "GET /a HTTP/1.1");
}

TEST(BufferTest, ReadFdReadsThroughPipe) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);

    // 坑：Linux 匿名管道默认容量 64KB，写端是阻塞的话，写超过容量且没人读会永久阻塞。
    // 所以这里用 32KB（< 64KB），既能触发 readv 的第二段 + 扩容路径，又不会自己把自己锁死。
    const std::string payload(32 * 1024, 'z');
    const ssize_t written = ::write(fds[1], payload.data(), payload.size());
    ASSERT_EQ(written, static_cast<ssize_t>(payload.size()));

    Buffer buf(1024);
    int saved = 0;
    ssize_t total = 0;
    while (true) {
        const ssize_t n = buf.readFd(fds[0], &saved);
        if (n <= 0) break;
        total += n;
    }
    EXPECT_EQ(total, static_cast<ssize_t>(payload.size()));
    EXPECT_EQ(buf.readableBytes(), payload.size());
    EXPECT_EQ(buf.retrieveAllAsString(), payload);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(BufferTest, WriteFdConsumesReadableBytes) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    Buffer buf;
    buf.append("ping", 4);
    int saved = 0;
    EXPECT_EQ(buf.writeFd(fds[1], &saved), 4);
    EXPECT_EQ(buf.readableBytes(), 0u);

    char out[8] = {0};
    EXPECT_EQ(::read(fds[0], out, sizeof out), 4);
    EXPECT_EQ(std::string(out, 4), "ping");
    ::close(fds[0]);
    ::close(fds[1]);
}
