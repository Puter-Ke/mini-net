#include "mini_net/ShortUrlService.h"

#include <chrono>
#include <cctype>

namespace mininet {

namespace {
constexpr char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr size_t kAlphabetSize = sizeof(kAlphabet) - 1;

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

std::string ShortUrlService::base62(uint64_t n) {
    // 从 1 开始计数，保证短码非空；这里给一个固定偏移，让首个短码看起来更"随机"一点
    n += 100000;
    std::string out;
    do {
        out.push_back(kAlphabet[n % kAlphabetSize]);
        n /= kAlphabetSize;
    } while (n > 0);
    return out;
}

bool ShortUrlService::validCode(const std::string& code) {
    if (code.empty() || code.size() > 8) return false;
    for (char c : code) {
        if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string ShortUrlService::shorten(const std::string& url) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = by_url_.find(url);
    if (it != by_url_.end()) {
        shorten_total_.fetch_add(1);
        return it->second;
    }

    std::string code;
    do {
        code = base62(++counter_);
    } while (by_code_.find(code) != by_code_.end());   // 理论上不会撞，防御性检查

    by_code_[code] = Entry{url, 0, nowSeconds()};
    by_url_[url] = code;
    shorten_total_.fetch_add(1);
    return code;
}

bool ShortUrlService::lookup(const std::string& code, Entry* out) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = by_code_.find(code);
    if (it == by_code_.end()) return false;
    ++it->second.hits;
    redirect_total_.fetch_add(1);
    if (out != nullptr) *out = it->second;
    return true;
}

bool ShortUrlService::stats(const std::string& code, Entry* out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = by_code_.find(code);
    if (it == by_code_.end()) return false;
    if (out != nullptr) *out = it->second;
    return true;
}

size_t ShortUrlService::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return by_code_.size();
}

}  // namespace mininet
