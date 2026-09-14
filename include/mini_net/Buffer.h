#pragma once
#include <sys/types.h>

#include <cstddef>
#include <string>
#include <vector>

namespace mininet {

// 应用层缓冲区（环形：读写下标在 vector 内滑动，支持空间复用与自动扩容）
// 面试点：为什么不直接用 std::string 拼接？
//   1) 每次 append 都可能重新分配 + 整体拷贝；
//   2) 取走数据后不能复用前面的空间（这里靠 prependable 空间 + memmove 复用）。
class Buffer {
public:
    explicit Buffer(size_t initial_size = 1024);

    size_t readableBytes() const { return write_idx_ - read_idx_; }
    size_t writableBytes() const { return buf_.size() - write_idx_; }
    size_t prependableBytes() const { return read_idx_; }

    const char* peek() const { return begin() + read_idx_; }
    const char* findCRLF() const;                 // 查找 \r\n，找不到返回 nullptr
    const char* findEOL() const;                  // 查找 \n

    void retrieve(size_t n);
    void retrieveAll();
    void retrieveUntil(const char* end);          // 取到 end（不含）
    std::string retrieveAsString(size_t n);
    std::string retrieveAllAsString();

    void append(const char* data, size_t n);
    void append(const std::string& s) { append(s.data(), s.size()); }

    // 一次 syscall 把数据读进缓冲区：readv + 栈上 64KB 备用空间
    // 好处：小包时不扩容（直接读进现有可写空间），大包时一次读满 64KB 再合并
    ssize_t readFd(int fd, int* saved_errno);
    ssize_t writeFd(int fd, int* saved_errno);

    void ensureWritable(size_t n);
    void shrink(size_t reserve);

private:
    char* begin() { return buf_.data(); }
    const char* begin() const { return buf_.data(); }
    void makeSpace(size_t n);

    std::vector<char> buf_;
    size_t read_idx_{0};
    size_t write_idx_{0};
};

}  // namespace mininet
