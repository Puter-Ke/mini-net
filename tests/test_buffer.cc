#include <gtest/gtest.h>

#include "mini_net/Buffer.h"

using namespace mininet;

// TODO(M4): 边写实现边补测试，覆盖率目标 ≥70%
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

// 待补：扩容后 peek 指针是否失效、retrieveAll 后空间能否复用、readFd 的 EAGAIN 处理
