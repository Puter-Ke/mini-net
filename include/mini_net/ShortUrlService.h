#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mininet {

// 短链业务：长链接 <-> 短码 的存储与生成
// 面试点：
//  - 短码怎么生成？这里用"自增计数 + base62 编码"，天然无碰撞、可预测长度；
//    分布式场景下要么给每台机器分配号段，要么用号段服务（美团 Leaf 那类）。
//  - 去重：同一 URL 重复提交返回同一短码，所以需要 url -> code 反向索引（用哈希表，O(1)）。
//  - 并发：加锁保护两张表。真实系统会分片锁或换 Redis/DB，这里保持简单可读。
class ShortUrlService {
public:
    struct Entry {
        std::string url;
        uint64_t hits{0};
        int64_t created_at{0};
    };

    std::string shorten(const std::string& url);          // 返回短码（已存在则复用）
    bool lookup(const std::string& code, Entry* out);     // 命中则 hits+1
    bool stats(const std::string& code, Entry* out) const;
    size_t size() const;

    uint64_t shortenTotal() const { return shorten_total_.load(); }
    uint64_t redirectTotal() const { return redirect_total_.load(); }

    static bool validCode(const std::string& code);

private:
    static std::string base62(uint64_t n);

    mutable std::mutex mtx_;
    std::unordered_map<std::string, Entry> by_code_;
    std::unordered_map<std::string, std::string> by_url_;
    uint64_t counter_{0};
    std::atomic<uint64_t> shorten_total_{0};
    std::atomic<uint64_t> redirect_total_{0};
};

}  // namespace mininet
