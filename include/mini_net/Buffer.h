#pragma once
#include <sys/types.h>   // ssize_t

#include <cstddef>
#include <string>
#include <vector>

namespace mininet {

// 应用层缓冲区：解决 TCP 粘包/拆包
// TODO(M4): 先用 std::vector<char> 实现，再换成环形缓冲（读写下标 + 自动扩容 + 复位）
class Buffer {
public:
    explicit Buffer(size_t initial = 1024);
    ~Buffer();

    size_t readableBytes() const;
    size_t writableBytes() const;
    const char* peek() const;

    void retrieve(size_t n);
    void retrieveAll();
    std::string retrieveAsString(size_t n);

    void append(const char* data, size_t n);

    // 关键优化点：一次 syscall 把数据直接读进缓冲区，避免 read 到临时栈再拷贝
    // 提示：readv + 栈上 extrabuf（典型 64KB），或用 MSG_PEEK + read
    ssize_t readFd(int fd, int* saved_errno);
    ssize_t writeFd(int fd, int* saved_errno);

private:
    // TODO(M4): 换成环形缓冲：读写下标 + 自动扩容 + 空间复用
    void ensureWritable(size_t n);

    std::vector<char> buf_;
    size_t read_idx_{0};
    size_t write_idx_{0};
};

}  // namespace mininet
