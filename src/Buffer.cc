#include "mini_net/Buffer.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace mininet {

Buffer::Buffer(size_t initial) : buf_(std::max<size_t>(initial, 64)) {}

Buffer::~Buffer() = default;

size_t Buffer::readableBytes() const { return write_idx_ - read_idx_; }

size_t Buffer::writableBytes() const { return buf_.size() - write_idx_; }

const char* Buffer::peek() const { return buf_.data() + read_idx_; }

void Buffer::retrieve(size_t n) {
    if (n >= readableBytes()) {
        retrieveAll();
        return;
    }
    read_idx_ += n;
}

void Buffer::retrieveAll() {
    read_idx_ = 0;
    write_idx_ = 0;
}

std::string Buffer::retrieveAsString(size_t n) {
    const size_t len = std::min(n, readableBytes());
    std::string out(peek(), len);
    retrieve(len);
    return out;
}

void Buffer::append(const char* data, size_t n) {
    ensureWritable(n);
    std::memcpy(buf_.data() + write_idx_, data, n);
    write_idx_ += n;
}

void Buffer::ensureWritable(size_t n) {
    if (writableBytes() >= n) return;
    // TODO(M4)：先看"已读空洞 + 尾部空闲"是否够用，够就 memmove 复位，不够才扩容
    std::vector<char> bigger(std::max(buf_.size() * 2, write_idx_ + n));
    std::memcpy(bigger.data(), buf_.data() + read_idx_, readableBytes());
    write_idx_ = readableBytes();
    read_idx_ = 0;
    buf_.swap(bigger);
}

// M1 版：一次 read 进缓冲区（够用）。M4 你要换成 readv + 栈上 64KB extrabuf，
// 并用环形缓冲复用空间，减少拷贝和扩容。
ssize_t Buffer::readFd(int fd, int* saved_errno) {
    if (writableBytes() < 1024) ensureWritable(65536);
    const ssize_t n = ::read(fd, buf_.data() + write_idx_, writableBytes());
    if (n > 0) {
        write_idx_ += static_cast<size_t>(n);
    } else if (n < 0 && saved_errno != nullptr) {
        *saved_errno = errno;
    }
    return n;
}

ssize_t Buffer::writeFd(int fd, int* saved_errno) {
    const ssize_t n = ::write(fd, peek(), readableBytes());
    if (n > 0) {
        retrieve(static_cast<size_t>(n));
    } else if (n < 0 && saved_errno != nullptr) {
        *saved_errno = errno;
    }
    return n;
}

}  // namespace mininet
