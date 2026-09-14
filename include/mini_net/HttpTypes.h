#pragma once
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace mininet {

struct HttpRequest {
    std::string method;
    std::string target;    // 原始请求目标，例如 /api/stats/aB3c?x=1
    std::string path;      // 去掉查询串
    std::string query;
    std::string version;   // HTTP/1.0 / HTTP/1.1
    std::vector<std::pair<std::string, std::string>> headers;   // key 已转小写
    std::string body;
    bool keep_alive{true};

    const std::string* findHeader(const std::string& key) const {
        for (const auto& kv : headers) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    bool hasHeader(const std::string& key) const { return findHeader(key) != nullptr; }
};

struct HttpResponse {
    int status{200};
    std::string content_type{"text/plain; charset=utf-8"};
    std::string body;
    std::vector<std::pair<std::string, std::string>> extra_headers;
    bool keep_alive{true};

    void setJson(const std::string& s) {
        content_type = "application/json; charset=utf-8";
        body = s;
    }

    static const char* reason(int code) {
        switch (code) {
            case 200: return "OK";
            case 201: return "Created";
            case 302: return "Found";
            case 400: return "Bad Request";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 408: return "Request Timeout";
            case 413: return "Payload Too Large";
            case 431: return "Request Header Fields Too Large";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            case 505: return "HTTP Version Not Supported";
            default: return "Unknown";
        }
    }

    std::string serialize() const {
        std::string out;
        out.reserve(body.size() + 256);
        char line[256];
        std::snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", status, reason(status));
        out += line;
        std::snprintf(line, sizeof line, "Content-Length: %zu\r\n", body.size());
        out += line;
        out += "Content-Type: " + content_type + "\r\n";
        out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        for (const auto& kv : extra_headers) out += kv.first + ": " + kv.second + "\r\n";
        out += "\r\n";
        out += body;
        return out;
    }
};

}  // namespace mininet
