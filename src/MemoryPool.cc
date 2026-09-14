#include "mini_net/MemoryPool.h"

#include <cstdint>
#include <cstdlib>
#include <new>

namespace mininet {

MemoryPool::MemoryPool(size_t block_size, size_t blocks_per_chunk)
    : block_size_(block_size < sizeof(Block)
                      ? sizeof(Block)
                      : ((block_size + alignof(std::max_align_t) - 1) /
                         alignof(std::max_align_t) * alignof(std::max_align_t))),
      blocks_per_chunk_(blocks_per_chunk == 0 ? 1 : blocks_per_chunk) {}

MemoryPool::~MemoryPool() {
    for (void* chunk : chunks_) std::free(chunk);
}

void MemoryPool::grow() {
    const size_t bytes = block_size_ * blocks_per_chunk_;
    auto* chunk = static_cast<char*>(std::malloc(bytes));
    if (chunk == nullptr) throw std::bad_alloc();

    chunks_.push_back(chunk);
    total_blocks_ += blocks_per_chunk_;

    // 把这一整块切成 blocks_per_chunk_ 个小块串成链表
    for (size_t i = 0; i < blocks_per_chunk_; ++i) {
        auto* block = reinterpret_cast<Block*>(chunk + i * block_size_);
        block->next = free_list_;
        free_list_ = block;
    }
}

void* MemoryPool::allocate() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (free_list_ == nullptr) grow();
    Block* block = free_list_;
    free_list_ = block->next;
    return block;
}

void MemoryPool::deallocate(void* p) {
    if (p == nullptr) return;
    std::lock_guard<std::mutex> lk(mtx_);
    auto* block = static_cast<Block*>(p);
    block->next = free_list_;
    free_list_ = block;
}

}  // namespace mininet
