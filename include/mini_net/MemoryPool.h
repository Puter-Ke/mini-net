#pragma once
#include <cstddef>
#include <mutex>
#include <vector>

namespace mininet {

// 定长对象池：块大小固定、按 chunk 批量申请、释放后进空闲链表复用。
// 面试点：相比 malloc 省在哪？
//   1) malloc 每次要走 size class 查找 + 可能的锁 + 空闲链表维护；
//      这里只是"取链表头"，O(1) 且没有元数据开销；
//   2) 同类对象连续分配，CPU 缓存局部性好、碎片少；
//   3) 代价：只适合固定大小，且内存不会归还给操作系统（本项目里 Conn 的大小是固定的，很合适）。
// 当前实现用互斥锁保护空闲链表；更进一步的做法是每线程一个池（thread_local）或 lock-free 栈。
class MemoryPool {
public:
    explicit MemoryPool(size_t block_size, size_t blocks_per_chunk = 256);
    ~MemoryPool();
    MemoryPool(const MemoryPool&) = delete;

    void* allocate();
    void deallocate(void* p);

    size_t blockSize() const { return block_size_; }
    size_t totalBlocks() const { return total_blocks_; }
    size_t chunks() const { return chunks_.size(); }

private:
    void grow();

    struct Block {
        Block* next;
    };

    const size_t block_size_;          // 已按 max_align_t 对齐
    const size_t blocks_per_chunk_;
    Block* free_list_{nullptr};
    std::vector<void*> chunks_;
    size_t total_blocks_{0};
    std::mutex mtx_;
};

// 给 STL 容器/allocate_shared 用的分配器适配器
template <typename T>
class PoolAllocator {
public:
    using value_type = T;

    explicit PoolAllocator(MemoryPool* pool) : pool_(pool) {}
    template <typename U>
    PoolAllocator(const PoolAllocator<U>& other) : pool_(other.pool()) {}   // NOLINT

    T* allocate(size_t n) {
        if (n == 1 && pool_ != nullptr) return static_cast<T*>(pool_->allocate());
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }
    void deallocate(T* p, size_t n) {
        if (n == 1 && pool_ != nullptr) {
            pool_->deallocate(p);
            return;
        }
        ::operator delete(p);
    }

    MemoryPool* pool() const { return pool_; }

    template <typename U>
    bool operator==(const PoolAllocator<U>& other) const { return pool_ == other.pool(); }
    template <typename U>
    bool operator!=(const PoolAllocator<U>& other) const { return pool_ != other.pool(); }

private:
    MemoryPool* pool_;
};

}  // namespace mininet
