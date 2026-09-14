#include "mini_net/ShortUrlApp.h"

#include <cstdio>

#include "mini_net/Logger.h"

namespace mininet {

namespace {

// 极简 JSON 字段提取（只支持 {"key":"value"} 这种扁平字符串，够用且无第三方依赖）
// 为什么不用 JSON 库？—— 项目初始目标之一是零第三方依赖；真实项目直接用 nlohmann/json 更好。
// 读 4 位十六进制
bool hex4(const std::string& s, size_t i, unsigned* out) {
    if (i + 4 > s.size()) return false;
    unsigned v = 0;
    for (int k = 0; k < 4; ++k) {
        const char c = s[i + static_cast<size_t>(k)];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= static_cast<unsigned>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= static_cast<unsigned>(c - 'A' + 10);
        } else {
            return false;
        }
    }
    *out = v;
    return true;
}

// 把码点按 UTF-8 追加（JSON 里 \u4e2d 这种转义必须还原成中文，否则回显会变成乱码）
void appendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool jsonExtractString(const std::string& body, const std::string& key, std::string* out, std::string* err) {
    // 结构校验：必须是 {...} 对象。否则 "{"url":"x" 这种被截断的 JSON 会被误判为合法
    size_t first = body.find_first_not_of(" \t\r\n");
    size_t last = body.find_last_not_of(" \t\r\n");
    if (first == std::string::npos || body[first] != '{' || body[last] != '}') {
        if (err) *err = "请求体不是合法的 JSON 对象";
        return false;
    }

    const std::string needle = "\"" + key + "\"";
    const size_t k = body.find(needle);
    if (k == std::string::npos) {
        if (err) *err = "缺少字段 " + key;
        return false;
    }
    size_t colon = body.find(':', k + needle.size());
    if (colon == std::string::npos) {
        if (err) *err = "字段格式非法";
        return false;
    }
    size_t i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
    if (i >= body.size() || body[i] != '"') {
        if (err) *err = "字段 " + key + " 必须是字符串";
        return false;
    }
    ++i;
    std::string value;
    while (i < body.size() && body[i] != '"') {
        if (body[i] == '\\' && i + 1 < body.size()) {
            ++i;
            switch (body[i]) {
                case 'n': value.push_back('\n'); break;
                case 't': value.push_back('\t'); break;
                case 'r': value.push_back('\r'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(body, i + 1, &cp)) {
                        if (err) *err = "\\u 转义不完整";
                        return false;
                    }
                    i += 4;
                    // 代理对（emoji 等）：\uD83D\uDE00 → 合并成一个码点
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < body.size() && body[i + 1] == '\\' &&
                        body[i + 2] == 'u') {
                        unsigned low = 0;
                        if (hex4(body, i + 3, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            i += 6;
                        }
                    }
                    appendUtf8(value, cp);
                    break;
                }
                default: value.push_back(body[i]); break;
            }
        } else {
            value.push_back(body[i]);
        }
        ++i;
    }
    if (i >= body.size()) {
        if (err) *err = "字符串没有结束引号";
        return false;
    }
    *out = value;
    return true;
}

bool validUrl(const std::string& url, std::string* err) {
    if (url.empty()) {
        if (err) *err = "url 不能为空";
        return false;
    }
    if (url.size() > 2048) {
        if (err) *err = "url 超过 2048 字节";
        return false;
    }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        if (err) *err = "只支持 http:// 或 https:// 开头的链接";
        return false;
    }
    return true;
}

void badRequest(HttpResponse* resp, const std::string& code, const std::string& detail) {
    resp->status = 400;
    resp->setJson("{\"error\":\"" + code + "\",\"detail\":\"" + detail + "\"}");
}

void notFound(HttpResponse* resp) {
    resp->status = 404;
    resp->setJson("{\"error\":\"not_found\",\"detail\":\"短码不存在\"}");
}

}  // namespace

void ShortUrlApp::handle(const HttpRequest& req, HttpResponse* resp) {
    const std::string& path = req.path;

    if (path == "/healthz") {
        if (req.method != "GET") {
            resp->status = 405;
            resp->extra_headers.emplace_back("Allow", "GET");
            resp->setJson("{\"error\":\"method_not_allowed\",\"detail\":\"/healthz 只支持 GET\"}");
            return;
        }
        resp->body = "ok";
        return;
    }

    if (path == "/metrics") {
        if (req.method != "GET") {
            resp->status = 405;
            resp->extra_headers.emplace_back("Allow", "GET");
            resp->setJson("{\"error\":\"method_not_allowed\",\"detail\":\"/metrics 只支持 GET\"}");
            return;
        }
        resp->content_type = "text/plain; charset=utf-8";
        resp->body = metrics_source_ ? metrics_source_() : std::string();
        return;
    }

    if (path == "/api/shorten") {
        if (req.method != "POST") {
            resp->status = 405;
            resp->extra_headers.emplace_back("Allow", "POST");
            resp->setJson("{\"error\":\"method_not_allowed\",\"detail\":\"/api/shorten 只支持 POST\"}");
            return;
        }
        handleShorten(req, resp);
        return;
    }

    const std::string stats_prefix = "/api/stats/";
    if (path.rfind(stats_prefix, 0) == 0) {
        if (req.method != "GET") {
            resp->status = 405;
            resp->extra_headers.emplace_back("Allow", "GET");
            resp->setJson("{\"error\":\"method_not_allowed\",\"detail\":\"该接口只支持 GET\"}");
            return;
        }
        handleStats(path.substr(stats_prefix.size()), resp);
        return;
    }

    // GET /{code}
    if (!path.empty() && path[0] == '/' && path.size() > 1) {
        if (req.method != "GET") {
            resp->status = 405;
            resp->extra_headers.emplace_back("Allow", "GET");
            resp->setJson("{\"error\":\"method_not_allowed\",\"detail\":\"短链跳转只支持 GET\"}");
            return;
        }
        handleRedirect(path.substr(1), resp);
        return;
    }

    notFound(resp);
}

void ShortUrlApp::handleShorten(const HttpRequest& req, HttpResponse* resp) {
    std::string url;
    std::string err;
    if (!jsonExtractString(req.body, "url", &url, &err)) {
        badRequest(resp, "invalid_url", err);
        return;
    }
    if (!validUrl(url, &err)) {
        badRequest(resp, "invalid_url", err);
        return;
    }

    const std::string code = service_.shorten(url);
    resp->status = 201;
    resp->setJson("{\"code\":\"" + code + "\",\"short\":\"http://" + host_ + "/" + code +
                  "\",\"url\":\"" + url + "\"}");
}

void ShortUrlApp::handleRedirect(const std::string& code, HttpResponse* resp) {
    if (!ShortUrlService::validCode(code)) {
        badRequest(resp, "invalid_code", "短码只允许 1-8 位字母数字");
        return;
    }
    ShortUrlService::Entry entry;
    if (!service_.lookup(code, &entry)) {
        notFound(resp);
        return;
    }
    resp->status = 302;
    resp->body.clear();
    resp->extra_headers.emplace_back("Location", entry.url);
    resp->extra_headers.emplace_back("Cache-Control", "no-store");
}

void ShortUrlApp::handleStats(const std::string& code, HttpResponse* resp) {
    if (!ShortUrlService::validCode(code)) {
        badRequest(resp, "invalid_code", "短码只允许 1-8 位字母数字");
        return;
    }
    ShortUrlService::Entry entry;
    if (!service_.stats(code, &entry)) {
        notFound(resp);
        return;
    }
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"code\":\"%s\",\"url\":\"%s\",\"hits\":%llu,\"created_at\":%lld}", code.c_str(),
                  entry.url.c_str(), static_cast<unsigned long long>(entry.hits),
                  static_cast<long long>(entry.created_at));
    resp->setJson(buf);
}

void ShortUrlApp::appendMetrics(std::string& out) const {
    char line[256];
    std::snprintf(line, sizeof line, "shorten_total %llu\n",
                  static_cast<unsigned long long>(service_.shortenTotal()));
    out += line;
    std::snprintf(line, sizeof line, "redirect_total %llu\n",
                  static_cast<unsigned long long>(service_.redirectTotal()));
    out += line;
    std::snprintf(line, sizeof line, "stored_codes %zu\n", service_.size());
    out += line;
}

}  // namespace mininet
