#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "mini_net/MemoryPool.h"

using namespace mininet;

namespace {
struct Small {
    char data[200];
};
}  // namespace

TEST(MemoryPoolTest, ReusesBlocksAndGrowsInChunks) {
    MemoryPool pool(sizeof(Small), 128);

    std::vector<void*> blocks;
    for (int i = 0; i < 300; ++i) blocks.push_back(pool.allocate());

    EXPECT_EQ(pool.totalBlocks(), 384u);          // 128 * 3
    EXPECT_EQ(pool.chunks(), 3u);

    // 全部归还后，再申请应该复用而不是重新 chunk
    for (void* p : blocks) pool.deallocate(p);
    const size_t chunks_before = pool.chunks();
    for (int i = 0; i < 300; ++i) blocks[i] = pool.allocate();
    EXPECT_EQ(pool.chunks(), chunks_before) << "归还的块没有被复用";
    for (void* p : blocks) pool.deallocate(p);
}

TEST(MemoryPoolTest, BlocksAreAlignedAndNonOverlapping) {
    MemoryPool pool(8, 64);   // 故意给一个很小的块大小，内部要往上对齐
    void* a = pool.allocate();
    void* b = pool.allocate();
    EXPECT_NE(a, b);
    EXPECT_GE(pool.blockSize(), sizeof(void*));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(a) % alignof(std::max_align_t), 0u);
    pool.deallocate(a);
    pool.deallocate(b);
}

// 定量对比：观察对象池相对 new/delete 的收益（数字会随机器变化，只用于趋势）
TEST(MemoryPoolTest, PoolIsFasterThanNewDelete) {
    constexpr int kOps = 300000;
    MemoryPool pool(sizeof(Small), 1024);

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        auto* p = new Small();
        p->data[0] = static_cast<char>(i);
        delete p;
    }
    const auto ns_new = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count() / kOps;

    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        auto* p = static_cast<Small*>(pool.allocate());
        p->data[0] = static_cast<char>(i);
        pool.deallocate(p);
    }
    const auto ns_pool = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - t1)
                             .count() / kOps;

    std::printf("[池对比] new/delete %.1f ns/次  vs  对象池 %.1f ns/次（%d 次操作）\n",
                static_cast<double>(ns_new), static_cast<double>(ns_pool), kOps);
    EXPECT_LT(ns_pool, ns_new) << "对象池没有比 new/delete 快（检查实现是否真的走了空闲链表）";
}
