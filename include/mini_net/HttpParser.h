#pragma once
#include <cstddef>
#include <string>

#include "mini_net/Buffer.h"
#include "mini_net/HttpTypes.h"

namespace mininet {

// HTTP/1.1 请求解析器（增量、状态机）
// 面试点：为什么必须增量？
//   TCP 是字节流，"一次 read" 可能只有半个请求（半包），也可能是两个半请求（粘包）。
//   所以解析器要能在数据不完整时返回 NeedMore 并保留状态，下批数据到了接着解析。
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

    Result parse(Buffer* buf, HttpRequest* req, Error* err);
    void reset();

private:
    Result parseRequestLine(Buffer* buf, HttpRequest* req, Error* err);
    Result parseHeaders(Buffer* buf, HttpRequest* req, Error* err);
    Result parseBody(Buffer* buf, HttpRequest* req, Error* err);
    static std::string lower(const std::string& s);
    static std::string trim(const std::string& s);

    enum class State { RequestLine, Headers, Body, Done };
    State state_{State::RequestLine};
    size_t content_length_{0};
    size_t header_bytes_{0};
};

}  // namespace mininet
