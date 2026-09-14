#include "mini_net/Buffer.h"

#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace mininet {

namespace {
constexpr size_t kInitialSize = 1024;
constexpr size_t kExtraBufSize = 65536;   // 栈上备用空间，一次 readv 最多多读 64KB
}  // namespace

Buffer::Buffer(size_t initial_size) : buf_(std::max<size_t>(initial_size, kInitialSize)) {}

const char* Buffer::findCRLF() const {
    const char* start = peek();
    const char* end = begin() + write_idx_;
    for (const char* p = start; p + 1 < end; ++p) {
        if (p[0] == '\r' && p[1] == '\n') return p;
    }
    return nullptr;
}

const char* Buffer::findEOL() const {
    const char* start = peek();
    const char* end = begin() + write_idx_;
    for (const char* p = start; p < end; ++p) {
        if (*p == '\n') return p;
    }
    return nullptr;
}

void Buffer::retrieve(size_t n) {
    if (n >= readableBytes()) {
        retrieveAll();
        return;
    }
    read_idx_ += n;
}

void Buffer::retrieveAll() {
    // 关键：不是清空 vector，而是把两个下标归零 —— 空间立刻可复用
    read_idx_ = 0;
    write_idx_ = 0;
}

void Buffer::retrieveUntil(const char* end) {
    if (end < peek()) return;
    retrieve(static_cast<size_t>(end - peek()) + 1);   // 连同分隔符一起取走
}

std::string Buffer::retrieveAsString(size_t n) {
    const size_t len = std::min(n, readableBytes());
    std::string out(peek(), len);
    retrieve(len);
    return out;
}

std::string Buffer::retrieveAllAsString() { return retrieveAsString(readableBytes()); }

void Buffer::append(const char* data, size_t n) {
    ensureWritable(n);
    std::memcpy(begin() + write_idx_, data, n);
    write_idx_ += n;
}

void Buffer::ensureWritable(size_t n) {
    if (writableBytes() >= n) return;
    makeSpace(n);
}

void Buffer::makeSpace(size_t n) {
    // 先把已读空间 + 尾部空间合起来看够不够（memmove 比重新分配便宜得多）
    if (prependableBytes() + writableBytes() < n + 1) {
        buf_.resize(write_idx_ + n);
    }
    const size_t readable = readableBytes();
    std::memmove(begin(), begin() + read_idx_, readable);
    read_idx_ = 0;
    write_idx_ = readable;
}

void Buffer::shrink(size_t reserve) {
    std::vector<char> fresh(std::max<size_t>(kInitialSize, readableBytes() + reserve));
    std::memcpy(fresh.data(), peek(), readableBytes());
    write_idx_ = readableBytes();
    read_idx_ = 0;
    buf_.swap(fresh);
}

ssize_t Buffer::readFd(int fd, int* saved_errno) {
    char extrabuf[kExtraBufSize];
    struct iovec vec[2];

    const size_t writable = writableBytes();
    vec[0].iov_base = begin() + write_idx_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof extrabuf;

    // 可写空间够大就只用一块，省掉一次合并拷贝
    const int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n < 0) {
        if (saved_errno) *saved_errno = errno;
    } else if (static_cast<size_t>(n) <= writable) {
        write_idx_ += static_cast<size_t>(n);
    } else {
        write_idx_ = buf_.size();
        append(extrabuf, static_cast<size_t>(n) - writable);
    }
    return n;
}

ssize_t Buffer::writeFd(int fd, int* saved_errno) {
    const ssize_t n = ::write(fd, peek(), readableBytes());
    if (n > 0) {
        retrieve(static_cast<size_t>(n));
    } else if (n < 0 && saved_errno) {
        *saved_errno = errno;
    }
    return n;
}

}  // namespace mininet
