#include "mini_net/HttpParser.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace mininet {

namespace {

// 去掉首尾空白
std::string strip(const std::string& s, size_t begin, size_t end) {
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) ++begin;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) --end;
    return s.substr(begin, end - begin);
}

}  // namespace

std::string HttpParser::lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string HttpParser::trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

void HttpParser::reset() {
    state_ = State::RequestLine;
    content_length_ = 0;
    header_bytes_ = 0;
}

HttpParser::Result HttpParser::parse(Buffer* buf, HttpRequest* req, Error* err) {
    while (true) {
        switch (state_) {
            case State::RequestLine: {
                const Result r = parseRequestLine(buf, req, err);
                if (r != Result::Complete) return r;
                state_ = State::Headers;
                break;
            }
            case State::Headers: {
                const Result r = parseHeaders(buf, req, err);
                if (r != Result::Complete) return r;
                if (content_length_ == 0) {
                    state_ = State::Done;
                } else {
                    state_ = State::Body;
                }
                break;
            }
            case State::Body: {
                const Result r = parseBody(buf, req, err);
                if (r != Result::Complete) return r;
                state_ = State::Done;
                break;
            }
            case State::Done:
                return Result::Complete;
        }
    }
}

HttpParser::Result HttpParser::parseRequestLine(Buffer* buf, HttpRequest* req, Error* err) {
    const char* eol = buf->findEOL();
    if (eol == nullptr) {
        header_bytes_ = buf->readableBytes();
        if (header_bytes_ > kMaxHeaderBytes) {
            err->status = 431;
            err->code = "headers_too_large";
            err->detail = "请求行或请求头超过 8 KiB";
            return Result::Error;
        }
        return Result::NeedMore;
    }

    const std::string line(buf->peek(), static_cast<size_t>(eol - buf->peek()));
    buf->retrieveUntil(eol + 1);
    header_bytes_ += line.size() + 1;

    if (line.empty() || line.find('\r') == std::string::npos) {
        // 裸 \n 结尾：宽容处理，但纯空行是非法请求
        if (line.empty()) {
            err->status = 400;
            err->code = "bad_request";
            err->detail = "请求行为空";
            return Result::Error;
        }
    }

    const std::string clean = trim(line);
    std::istringstream iss(clean);
    std::string method;
    std::string target;
    std::string version;
    if (!(iss >> method >> target >> version)) {
        err->status = 400;
        err->code = "bad_request";
        err->detail = "请求行格式非法（应为 METHOD TARGET VERSION）";
        return Result::Error;
    }

    // 版本校验：只支持 HTTP/1.0 和 HTTP/1.1
    if (version.rfind("HTTP/", 0) != 0) {
        err->status = 400;
        err->code = "bad_request";
        err->detail = "缺少 HTTP 版本";
        return Result::Error;
    }
    const std::string ver = version.substr(5);
    if (ver != "1.0" && ver != "1.1") {
        err->status = 505;
        err->code = "unsupported_version";
        err->detail = "只支持 HTTP/1.0 与 HTTP/1.1";
        return Result::Error;
    }

    req->method = method;
    req->target = target;
    req->version = version;
    const size_t q = target.find('?');
    if (q == std::string::npos) {
        req->path = target;
    } else {
        req->path = target.substr(0, q);
        req->query = target.substr(q + 1);
    }
    // 默认是否长连接：HTTP/1.1 默认 keep-alive，HTTP/1.0 默认关闭
    req->keep_alive = (version == "HTTP/1.1");
    return Result::Complete;
}

HttpParser::Result HttpParser::parseHeaders(Buffer* buf, HttpRequest* req, Error* err) {
    while (true) {
        const char* eol = buf->findEOL();
        if (eol == nullptr) {
            header_bytes_ += buf->readableBytes();
            if (header_bytes_ > kMaxHeaderBytes) {
                err->status = 431;
                err->code = "headers_too_large";
                err->detail = "请求头超过 8 KiB";
                return Result::Error;
            }
            return Result::NeedMore;
        }

        const std::string raw(buf->peek(), static_cast<size_t>(eol - buf->peek()));
        buf->retrieveUntil(eol + 1);
        header_bytes_ += raw.size() + 1;

        if (header_bytes_ > kMaxHeaderBytes) {
            err->status = 431;
            err->code = "headers_too_large";
            err->detail = "请求头超过 8 KiB";
            return Result::Error;
        }

        const std::string line = trim(raw);
        if (line.empty()) {
            // 空行 = 请求头结束
            if (req->hasHeader("transfer-encoding")) {
                const std::string* te = req->findHeader("transfer-encoding");
                if (te != nullptr && lower(*te).find("chunked") != std::string::npos) {
                    err->status = 501;
                    err->code = "unsupported_transfer_encoding";
                    err->detail = "v1 暂不支持 chunked 请求体";
                    return Result::Error;
                }
            }
            const std::string* cl = req->findHeader("content-length");
            if (cl != nullptr) {
                char* endp = nullptr;
                const long v = std::strtol(cl->c_str(), &endp, 10);
                if (endp == cl->c_str() || v < 0) {
                    err->status = 400;
                    err->code = "bad_request";
                    err->detail = "Content-Length 非法";
                    return Result::Error;
                }
                if (static_cast<size_t>(v) > kMaxBodyBytes) {
                    err->status = 413;
                    err->code = "payload_too_large";
                    err->detail = "请求体超过 8 KiB";
                    return Result::Error;
                }
                content_length_ = static_cast<size_t>(v);
            }
            const std::string* conn = req->findHeader("connection");
            if (conn != nullptr) {
                const std::string c = lower(trim(*conn));
                if (c == "close") {
                    req->keep_alive = false;
                } else if (c == "keep-alive") {
                    req->keep_alive = true;
                }
            }
            return Result::Complete;
        }

        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            err->status = 400;
            err->code = "bad_request";
            err->detail = "请求头缺少冒号";
            return Result::Error;
        }
        req->headers.emplace_back(lower(trim(line.substr(0, colon))), trim(line.substr(colon + 1)));
    }
}

HttpParser::Result HttpParser::parseBody(Buffer* buf, HttpRequest* req, Error* err) {
    (void)err;
    if (buf->readableBytes() < content_length_) return Result::NeedMore;
    req->body = buf->retrieveAsString(content_length_);
    return Result::Complete;
}

}  // namespace mininet
