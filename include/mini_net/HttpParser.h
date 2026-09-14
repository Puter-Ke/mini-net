#pragma once
#include <cstddef>
#include <string>

#include "mini_net/Buffer.h"
#include "mini_net/HttpTypes.h"

namespace mininet {

// HTTP/1.1 请求解析器（增量、状态机）
//
// 面试点 1：为什么必须增量？
//   TCP 是字节流，"一次 read" 可能只有半个请求（半包），也可能是两个半请求（粘包）。
//   解析器要能在数据不完整时返回 NeedMore、保留状态，下批数据到了接着解析。
//
// 面试点 2（本项目踩过的真 bug）：**请求对象必须由解析器持有，而不是每次调用由外部新建**。
//   因为增量解析会把"请求行"和"请求头/请求体"分在多次调用里处理；
//   如果每次调用都传一个新对象，先前解析出的 method/target 就丢了，
//   结果是一个"方法为空"的请求 → 路由失败 → 偶发 404。
//   结构上的修法就是让解析器拥有 req_，reset() 时一并清空。
class HttpParser {
public:
    enum class Result { NeedMore, Complete, Error };

    struct Error {
        int status{400};
        std::string code{"bad_request"};
        std::string detail;
    };

    static constexpr size_t kMaxHeaderBytes = 8192;
    static constexpr size_t kMaxBodyBytes = 8192;

    Result parse(Buffer* buf, Error* err);

    // 解析出完整请求后从这里取（parse 返回 Complete 时有效）
    const HttpRequest& request() const { return req_; }
    HttpRequest& request() { return req_; }

    void reset();   // 一个请求处理完，准备解析下一个

private:
    Result parseRequestLine(Buffer* buf, Error* err);
    Result parseHeaders(Buffer* buf, Error* err);
    Result parseBody(Buffer* buf, Error* err);
    static std::string lower(const std::string& s);
    static std::string trim(const std::string& s);

    enum class State { RequestLine, Headers, Body, Done };
    State state_{State::RequestLine};
    HttpRequest req_;              // ← 增量过程中的请求对象，跨多次 parse 调用存活
    size_t content_length_{0};
    size_t header_bytes_{0};
};

}  // namespace mininet
