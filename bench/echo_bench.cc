// bench/echo_bench.cc —— mini-net 自写压测客户端（第 6 周 M6）
//
// 为什么要自己写压测工具，而不是用 ab / wrk？
//   1. ab 一次只跑一条连接、一个请求一个往返，测不出「长连接复用」下的真实吞吐；
//   2. wrk 很快但只认 HTTP，测不了 mini-net 最早的回显协议，也不方便改；
//   3. 自己写一遍才知道 QPS 和 P50/P99 这两个数字到底是怎么算出来的——
//      面试官问「你这个 29k QPS 是怎么测的」，你得能讲清楚每一个细节。
//
// 实现要点（全篇只依赖 C++17 标准库 + POSIX，不引入任何第三方库）：
//   1. 非阻塞 connect + epoll。客户端也是事件驱动的：一个线程、一个 epoll，
//      同时驱动上千条连接，不需要线程池（压测客户端不该比服务端还重）。
//   2. 每条连接复用到底（HTTP 走 keep-alive，回显协议就是一条长连接），
//      这样测出来的才是「连接复用」的吞吐，而不是「每个请求建一次连接」的吞吐。
//   3. 支持 pipeline：一次连续发出 P 个请求再收响应，用来压低 RTT 的影响。
//      注意开了 pipeline 之后延迟会被排队时间污染，这是正常的，见 bench/README.md。
//   4. 延迟统计：请求发出前把时间戳塞进该连接的队列，收到完整响应时出队相减，
//      统计的是端到端往返延迟，内部用纳秒，输出换算成微秒。
//   5. 起跑线（barrier）：等所有连接都建立好才开始发请求，避免「前面的连接已经在打、
//      后面的连接还在连」把测量窗口拉长（这一点和 scripts/bench_quick.py 的做法一致）。
//   6. 客户端本地端口耗尽时给出人能看懂的排查指引——同机大并发压测最容易踩的坑。
//
// 编译（bench/CMakeLists.txt 里已经有这个目标，不用改构建脚本）：
//   cmake --build build -j            # 直接编出 build/echo_bench
// 也可以手动编译：
//   g++ -std=c++17 -O2 -pthread bench/echo_bench.cc -o echo_bench
//
// 用法示例：
//   ./echo_bench --host 127.0.0.1 --port 8080 --conns 1000 --requests 1000 --size 64
//   ./echo_bench --mode http --path /healthz --conns 1000 --requests 1000
//   ./echo_bench --mode http --method POST --path /api/shorten \
//                --body '{"url":"https://example.com/bench/%d"}' --conns 200
//   ./echo_bench --mode http --path /healthz --duration 30 --conns 1000   # 定压 30 秒
//   ./echo_bench --help

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 一些常量。写死在这里，是为了让热点路径上少几个判断。
// ---------------------------------------------------------------------------

// epoll_wait 一次最多取回多少个事件。4096 够大，又不会让事件数组占太多内存。
constexpr int kMaxEvents = 4096;

// 单次事件里最多循环读 / 写多少轮，防止一条特别活跃的连接把别的连接饿死。
constexpr int kReadBudget = 16;
constexpr int kWriteBudget = 64;

// 一条响应头最多能有多大。超过就认为服务端行为异常，直接报错，
// 而不是让接收缓冲区无限涨下去（压测工具自己 OOM 就太难看了）。
constexpr size_t kMaxHeaderBytes = 64 * 1024;

// 最多记录多少个延迟样本（每个 8 字节，400 万条约 32 MB）。
// 定压（--duration）模式下请求数可能上亿，全记下来会吃光内存，所以设个上限。
constexpr size_t kMaxLatencySamples = 4000000;

// 每连接发送缓冲（积压的请求）上限。size × pipeline 超过它就直接报错，
// 免得内存被一堆没发出去的大请求撑爆。
constexpr size_t kMaxOutBuffer = 8u * 1024 * 1024;

// 周期性扫描（超时检查、收尾检查）的间隔。O(连接数) 的活不能每个事件轮次都干一遍。
constexpr uint64_t kScanIntervalNs = 100ull * 1000 * 1000;   // 100 毫秒

// 取单调时钟（纳秒）。用 CLOCK_MONOTONIC 而不是 CLOCK_REALTIME：
// 后者会被校时/改系统时间影响，测延迟时可能算出负数。
uint64_t nowNs() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// 数字加千位分隔符，输出报告时更好读（1234567 -> 1,234,567）
std::string withCommas(long long v) {
    const bool neg = v < 0;
    const unsigned long long u =
        neg ? static_cast<unsigned long long>(-v) : static_cast<unsigned long long>(v);
    const std::string digits = std::to_string(u);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3 + 1);
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return neg ? "-" + out : out;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// 把 %d 替换成连接序号，%% 还原成一个 %。这样每条连接可以发不同的 URL，例如
//   --body '{"url":"https://example.com/bench/%d"}'
// 用来压测「插入新短链」这条路径，而不是一直命中同一个已经存在的 URL。
std::string expandPlaceholder(const std::string& s, int conn_index) {
    std::string out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 1 < s.size()) {
            if (s[i + 1] == 'd') {
                out += std::to_string(conn_index);
                ++i;
                continue;
            }
            if (s[i + 1] == '%') {
                out.push_back('%');
                ++i;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// 回显模式的请求内容：a~z 循环填充。用有规律的内容（而不是一坨随机字节），
// 这样回显校验能立刻发现少发、发错、串包这类问题。
std::string makeEchoPayload(int size) {
    std::string p;
    p.resize(static_cast<size_t>(size));
    for (int i = 0; i < size; ++i) {
        p[static_cast<size_t>(i)] = static_cast<char>('a' + (i % 26));
    }
    return p;
}

// ---------------------------------------------------------------------------
// 命令行参数
// ---------------------------------------------------------------------------

struct Options {
    std::string host = "127.0.0.1";
    int port = 8080;
    int conns = 100;
    long requests = 1000;             // 每条连接发多少个请求
    int size = 64;                    // echo 模式：每条消息多少字节
    std::string mode = "echo";        // echo | http
    int pipeline = 1;                 // 一次在途多少个请求
    std::string path;                 // http 模式请求路径，默认 /healthz
    std::string method;               // 默认按路径推断：/api/shorten 用 POST，其余 GET
    std::string body;                 // POST 的请求体，支持 %d 占位符
    std::vector<int> expect_status;   // 期望状态码，默认按方法推断
    int timeout_ms = 5000;            // 单条连接多久没进展算失败
    int connect_timeout_ms = 5000;    // 连接建立多久没完成算失败
    double duration = 0.0;            // > 0 时按时间定压（忽略 --requests）
    int source_ip_count = 1;          // 用几个源地址（回环时是 127.0.0.1 ~ 127.0.0.N）
    bool reuse_port = true;           // 客户端 socket 开 SO_REUSEPORT
    bool nodelay = false;             // 开 TCP_NODELAY（默认不开，方便公平对比 Nagle）
    bool verify = true;               // echo 模式校验回显内容
    std::string label;                // 报告里的标签，方便区分不同轮次
    std::string json_out;             // 额外写一份 JSON 结果到这里
};

void printUsage(const char* prog) {
    std::printf(
        "mini-net 压测客户端（自写：非阻塞 connect + epoll，不依赖任何第三方库）\n"
        "\n"
        "用法: %s [选项]\n"
        "\n"
        "目标与负载:\n"
        "  --host <地址>          服务端地址，默认 127.0.0.1（只支持 IPv4）\n"
        "  --port <端口>          服务端端口，默认 8080\n"
        "  --conns <N>            并发连接数，默认 100\n"
        "  --requests <N>         每条连接发多少个请求，默认 1000；填 0 表示改用 --duration\n"
        "  --pipeline <N>         一次在途多少个请求（HTTP 流水线），默认 1\n"
        "  --duration <秒>        定压时长（比如 30）；大于 0 时忽略 --requests，跑满这段时间\n"
        "  --size <字节>          echo 模式每条消息的字节数，默认 64\n"
        "\n"
        "协议:\n"
        "  --mode echo|http       默认 echo（回显协议，发什么收什么）\n"
        "  --path <路径>          http 模式请求路径，默认 /healthz\n"
        "  --method <方法>        http 模式请求方法；默认 /api/shorten 用 POST，其余用 GET\n"
        "  --body <内容>          POST 的请求体，支持 %%d 占位符（替换成连接序号）\n"
        "                         例如: --body '{\"url\":\"https://example.com/b/%%d\"}'\n"
        "  --expect-status <码>   期望的状态码，可用逗号分隔多个（如 302,200）；\n"
        "                         默认 GET 期望 200，POST 期望 201\n"
        "\n"
        "健壮性与环境:\n"
        "  --timeout-ms <毫秒>          单条连接多久没有进展算失败，默认 5000\n"
        "  --connect-timeout-ms <毫秒>  连接建立超时，默认 5000\n"
        "  --source-ip-count <N>  用 N 个源地址轮转（回环目标时是 127.0.0.1~127.0.0.N），\n"
        "                         默认 1。大并发下靠它绕开客户端本地端口耗尽\n"
        "  --no-reuse-port        关掉客户端 socket 的 SO_REUSEPORT\n"
        "  --nodelay              打开 TCP_NODELAY（关掉 Nagle 算法）\n"
        "  --no-verify            echo 模式不校验回显内容（省一点 CPU，但丢掉正确性检查）\n"
        "\n"
        "输出:\n"
        "  --label <文字>         报告标题，例如 baseline-4thread\n"
        "  --json <文件>          额外把结果写成 JSON（方便 CI 和画图）\n"
        "  -h, --help             显示这份帮助\n"
        "\n"
        "例子:\n"
        "  # 回显协议：1000 条并发连接，每条连接 1000 个请求\n"
        "  ./echo_bench --conns 1000 --requests 1000 --size 64\n"
        "\n"
        "  # HTTP 健康检查接口，定压 30 秒\n"
        "  ./echo_bench --mode http --path /healthz --conns 1000 --duration 30\n"
        "\n"
        "  # 压测短链创建（每条连接用不同的 URL，避免全部命中同一个已存在的短码）\n"
        "  ./echo_bench --mode http --method POST --path /api/shorten \\\n"
        "               --body '{\"url\":\"https://example.com/bench/%%d\"}' --conns 200\n"
        "\n"
        "  # 压测短链跳转（先创建拿到短码，把短码填进 --path；跳转是 302，要改期望状态码）\n"
        "  ./echo_bench --mode http --path /aB3cD --expect-status 302 --conns 1000\n",
        prog);
}

// 解析一个整数并检查范围。返回 false 表示参数不合法。
bool parseIntArg(const char* name, const char* s, long lo, long hi, long& out, std::string& err) {
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(s, &end, 10);
    if (errno != 0 || end == s || (end != nullptr && *end != '\0')) {
        err = std::string("参数 ") + name + " 需要一个整数，收到的是 \"" + s + "\"";
        return false;
    }
    if (v < lo || v > hi) {
        err = std::string("参数 ") + name + " 超出范围，应该在 " + std::to_string(lo) + " ~ " +
              std::to_string(hi) + " 之间，收到 " + std::to_string(v);
        return false;
    }
    out = v;
    return true;
}

bool parseArgs(int argc, char** argv, Options& o, std::string& err) {
    static struct option kLongOpts[] = {
        {"host",               required_argument, nullptr, 1000},
        {"port",               required_argument, nullptr, 1001},
        {"conns",              required_argument, nullptr, 1002},
        {"requests",           required_argument, nullptr, 1003},
        {"size",               required_argument, nullptr, 1004},
        {"mode",               required_argument, nullptr, 1005},
        {"pipeline",           required_argument, nullptr, 1006},
        {"path",               required_argument, nullptr, 1007},
        {"method",             required_argument, nullptr, 1008},
        {"body",               required_argument, nullptr, 1009},
        {"expect-status",      required_argument, nullptr, 1010},
        {"timeout-ms",         required_argument, nullptr, 1011},
        {"duration",           required_argument, nullptr, 1012},
        {"source-ip-count",    required_argument, nullptr, 1013},
        {"no-reuse-port",      no_argument,       nullptr, 1014},
        {"nodelay",            no_argument,       nullptr, 1015},
        {"no-verify",          no_argument,       nullptr, 1016},
        {"label",              required_argument, nullptr, 1017},
        {"json",               required_argument, nullptr, 1018},
        {"connect-timeout-ms", required_argument, nullptr, 1019},
        {"help",               no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    long v = 0;
    optind = 1;
    for (;;) {
        const int c = ::getopt_long(argc, argv, "h", kLongOpts, nullptr);
        if (c == -1) break;
        switch (c) {
            case 'h':
                printUsage(argv[0]);
                std::exit(0);
            case 1000:
                o.host = optarg;
                break;
            case 1001:
                if (!parseIntArg("--port", optarg, 1, 65535, v, err)) return false;
                o.port = static_cast<int>(v);
                break;
            case 1002:
                if (!parseIntArg("--conns", optarg, 1, 1000000, v, err)) return false;
                o.conns = static_cast<int>(v);
                break;
            case 1003:
                if (!parseIntArg("--requests", optarg, 0, 1000000000L, v, err)) return false;
                o.requests = v;
                break;
            case 1004:
                if (!parseIntArg("--size", optarg, 1, 1024 * 1024, v, err)) return false;
                o.size = static_cast<int>(v);
                break;
            case 1005:
                o.mode = toLower(optarg);
                break;
            case 1006:
                if (!parseIntArg("--pipeline", optarg, 1, 4096, v, err)) return false;
                o.pipeline = static_cast<int>(v);
                break;
            case 1007:
                o.path = optarg;
                break;
            case 1008:
                o.method = optarg;
                break;
            case 1009:
                o.body = optarg;
                break;
            case 1010: {
                // 允许 "302,200" 这种写法
                const std::string s = optarg;
                size_t pos = 0;
                for (;;) {
                    const size_t comma = s.find(',', pos);
                    const std::string one = trim(
                        s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
                    if (!one.empty()) {
                        if (!parseIntArg("--expect-status", one.c_str(), 100, 599, v, err)) return false;
                        o.expect_status.push_back(static_cast<int>(v));
                    }
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
                break;
            }
            case 1011:
                if (!parseIntArg("--timeout-ms", optarg, 1, 3600000, v, err)) return false;
                o.timeout_ms = static_cast<int>(v);
                break;
            case 1012: {
                char* end = nullptr;
                errno = 0;
                const double d = std::strtod(optarg, &end);
                if (errno != 0 || end == optarg || (end != nullptr && *end != '\0') || d < 0.0 || d > 86400.0) {
                    err = std::string("参数 --duration 需要一个 0 ~ 86400 的秒数，收到的是 \"") + optarg + "\"";
                    return false;
                }
                o.duration = d;
                break;
            }
            case 1013:
                if (!parseIntArg("--source-ip-count", optarg, 1, 254, v, err)) return false;
                o.source_ip_count = static_cast<int>(v);
                break;
            case 1014:
                o.reuse_port = false;
                break;
            case 1015:
                o.nodelay = true;
                break;
            case 1016:
                o.verify = false;
                break;
            case 1017:
                o.label = optarg;
                break;
            case 1018:
                o.json_out = optarg;
                break;
            case 1019:
                if (!parseIntArg("--connect-timeout-ms", optarg, 1, 3600000, v, err)) return false;
                o.connect_timeout_ms = static_cast<int>(v);
                break;
            default:
                err = "命令行里有不认识的参数，用 --help 看看支持哪些选项";
                return false;
        }
    }

    // ---- 参数之间的一致性检查 ----
    if (o.mode != "echo" && o.mode != "http") {
        err = "--mode 只能是 echo 或 http，收到的是 \"" + o.mode + "\"";
        return false;
    }
    if (o.requests == 0 && o.duration <= 0.0) {
        err = "--requests 0 表示定压模式，必须同时给 --duration（例如 --duration 30）";
        return false;
    }
    if (static_cast<size_t>(o.size) * static_cast<size_t>(o.pipeline) > kMaxOutBuffer) {
        err = "--size 乘 --pipeline 太大了（超过 8 MiB），每条连接的发送缓冲会撑爆内存，"
              "请调小 --size 或 --pipeline";
        return false;
    }

    // http 模式的默认值：路径、方法、body、期望状态码
    if (o.mode == "http") {
        if (o.path.empty()) o.path = "/healthz";
        if (o.path[0] != '/') o.path = "/" + o.path;
        if (o.method.empty()) o.method = (o.path.rfind("/api/shorten", 0) == 0) ? "POST" : "GET";
        std::transform(o.method.begin(), o.method.end(), o.method.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (o.method == "POST" && o.body.empty()) {
            // 给个能直接用的默认值，省得每次都要写一大串 JSON
            o.body = "{\"url\":\"https://example.com/bench\"}";
        }
        if (o.expect_status.empty()) {
            o.expect_status.push_back(o.method == "POST" ? 201 : 200);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 连接状态机
// ---------------------------------------------------------------------------

enum class ConnState {
    Connecting,   // 非阻塞 connect 已经发出去，等结果
    Waiting,      // 连上了，但在等起跑线（所有连接都建好才一起开打）
    Ready,        // 正在收发数据
    Done,         // 正常收工（该发的发完了，响应也收齐了）
    Failed,       // 出错了（建连失败 / 超时 / 协议不对）
};

struct Conn {
    int fd = -1;
    uint32_t ev_mask = 0;          // 当前在 epoll 里注册的事件，用来避免重复 epoll_ctl
    ConnState state = ConnState::Connecting;
    std::string src_ip;            // 这条连接绑的源地址（空串表示不显式 bind）
    std::string req;               // 这条连接要反复发的请求字节

    // 发送侧：out 里是「要发但还没发完」的字节，out_off 是已经发出去的部分
    std::string out;
    size_t out_off = 0;

    // 接收侧（http 模式）：in 里是收到但还没解析完的字节，in_off 是已消费的部分
    std::string in;
    size_t in_off = 0;

    long sent_req = 0;             // 已发出的请求数
    long done_req = 0;             // 已完整收到响应的请求数
    long inflight = 0;             // 已发出但还没收到响应（在途）

    // 在途请求的发送时刻队列（先进先出）。用 head 下标出队，避免每次 erase 搬移整块内存。
    std::vector<uint64_t> ts;
    size_t ts_head = 0;

    uint64_t last_progress = 0;    // 最后一次有进展的时刻（用来判断超时）
    uint64_t echo_recv = 0;        // echo 模式：累计收到多少字节（用来对齐校验的偏移）
    uint64_t echo_partial = 0;     // echo 模式：当前这个响应还差多少字节收满
};

enum class ParseResult { NeedMore, Complete, Error };

// ---------------------------------------------------------------------------
// 压测主体
// ---------------------------------------------------------------------------

class Bench {
  public:
    explicit Bench(const Options& opts) : opts_(opts) {}

    // 返回进程退出码：0 全部成功，2 是环境/参数问题，3 是跑完了但有失败
    int run();

  private:
    // ---- 初始化 ----
    bool resolveTarget(std::string& err);
    bool preflight(std::string& err);
    void prepareSourceIps(std::string& warn);
    void warnPortBudget();
    bool setupConns(std::string& err);
    bool startConnect(size_t index, std::string& err);
    std::string buildHttpRequest(int conn_index) const;

    // ---- 事件循环 ----
    void onEvent(Conn& c, uint32_t events);
    bool doRead(Conn& c);
    void pump(Conn& c);
    bool flushOut(Conn& c);
    void refill(Conn& c);
    bool moreRequests(const Conn& c) const;
    void maybeFinish(Conn& c);
    void maybeStart();
    void periodicScan();
    void checkTimeouts();
    void checkFinishes();

    // ---- 状态变更 ----
    void closeConn(Conn& c, bool success);
    void failConn(Conn& c, const std::string& category, const std::string& detail = std::string());
    void updateEvents(Conn& c);
    bool alive(const Conn& c) const {
        return c.fd >= 0 && c.state != ConnState::Done && c.state != ConnState::Failed;
    }

    // ---- 响应处理 ----
    ParseResult parseOne(Conn& c, int& status, std::string& err);
    bool drainHttp(Conn& c);
    void onResponseComplete(Conn& c, int status);
    bool verifyChunk(uint64_t base, const char* buf, size_t n) const;

    // ---- 结果 ----
    void report();
    void writeJson();

    Options opts_;
    bool http_mode_ = false;
    bool duration_mode_ = false;

    int epfd_ = -1;
    std::vector<epoll_event> events_;

    // 目标地址
    sockaddr_in target_;
    socklen_t target_len_ = 0;
    bool target_is_loopback_ = false;
    std::string host_header_;
    std::vector<std::string> src_ips_;
    std::string echo_payload_;

    std::vector<std::unique_ptr<Conn>> conns_;
    size_t alive_count_ = 0;        // 还没结束的连接数（Done/Failed 都减掉）
    int connecting_count_ = 0;      // 还在建连的连接数

    // 计时与起跑线
    bool started_ = false;
    uint64_t setup_start_ns_ = 0;
    uint64_t t0_ns_ = 0;
    uint64_t t1_ns_ = 0;
    uint64_t deadline_ns_ = 0;
    uint64_t last_scan_ns_ = 0;
    uint64_t timeout_ns_ = 0;
    uint64_t connect_timeout_ns_ = 0;

    // 统计
    long long ok_ = 0;              // 收到完整响应，且状态码符合预期
    long long bad_status_ = 0;      // 收到完整响应，但状态码不符合预期
    long long failed_ = 0;          // 没拿到响应的请求数（按连接折算）
    long long conn_failed_ = 0;     // 出错的连接条数
    long long lat_dropped_ = 0;     // 因为超过样本上限而没记下来的延迟
    std::vector<uint64_t> lat_ns_;
    std::map<int, long long> status_hist_;
    std::map<std::string, long long> err_hist_;
    std::string first_error_;
    uint64_t total_bytes_in_ = 0;
    uint64_t total_bytes_out_ = 0;

    // 进程资源用量
    struct rusage ru0_;
    struct rusage ru1_;
};

// ---- 地址解析与预检 --------------------------------------------------------

bool Bench::resolveTarget(std::string& err) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          // 只支持 IPv4：多源地址那套技巧依赖回环网段
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    const std::string port_str = std::to_string(opts_.port);
    const int rc = ::getaddrinfo(opts_.host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
        err = "解析地址失败：" + opts_.host + "（" + std::string(::gai_strerror(rc)) + "）";
        return false;
    }
    if (res == nullptr) {
        err = "解析地址失败：" + opts_.host + " 没有解析出任何 IPv4 地址";
        return false;
    }
    std::memcpy(&target_, res->ai_addr, sizeof(sockaddr_in));
    target_len_ = static_cast<socklen_t>(res->ai_addrlen);
    ::freeaddrinfo(res);

    // 判断是不是回环地址：决定能不能用「多源地址」那一招（127.0.0.0/8 整段都在本机）
    const uint32_t host_order = ntohl(target_.sin_addr.s_addr);
    target_is_loopback_ = ((host_order >> 24) == 127u);

    host_header_ = opts_.host + ":" + std::to_string(opts_.port);
    return true;
}

// 先拿一条普通阻塞连接探一下路：
//   - 服务端没起来 -> 立刻给出「先启动服务」的提示，而不是让 1000 条连接一起失败
//   - 地址/端口不通 -> 及时报错，不浪费时间
bool Bench::preflight(std::string& err) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = std::string("创建探测 socket 失败：") + std::strerror(errno);
        return false;
    }
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&target_), target_len_);
    const int saved = errno;
    ::close(fd);

    if (rc == 0) return true;

    if (saved == ECONNREFUSED) {
        err = "连接被拒绝： " + opts_.host + ":" + std::to_string(opts_.port) +
              " 上没有服务在监听。\n    先启动服务端： ./build/mini_net_server " +
              std::to_string(opts_.port);
    } else if (saved == ETIMEDOUT || saved == EHOSTUNREACH || saved == ENETUNREACH) {
        err = "连不上 " + opts_.host + ":" + std::to_string(opts_.port) + "：" +
              std::strerror(saved) + "\n    检查地址端口对不对、防火墙有没有拦。";
    } else {
        err = std::string("预检连接失败：") + std::strerror(saved);
    }
    return false;
}

// 决定用哪些源地址。回环目标时 127.0.0.0/8 整段都是本机地址，
// 所以可以轮流用 127.0.0.1、127.0.0.2……每个源地址都有一份独立的临时端口空间。
// 这是同机压到几万条连接时唯一管用的办法：SO_REUSEPORT 解决不了「同一个四元组」的问题。
void Bench::prepareSourceIps(std::string& warn) {
    src_ips_.clear();
    if (!target_is_loopback_) {
        if (opts_.source_ip_count > 1) {
            warn = "目标是 " + opts_.host +
                   "（不是回环地址），--source-ip-count 只在回环目标下有效，本次按 1 个源地址跑。\n"
                   "    想用多源地址，需要先给网卡加别名地址，例如：\n"
                   "    sudo ip addr add 192.168.1.100/32 dev eth0";
        }
        src_ips_.push_back("");     // 空串 = 不显式 bind，让内核自己挑源地址
        return;
    }
    const int n = std::max(1, opts_.source_ip_count);
    for (int i = 0; i < n; ++i) {
        src_ips_.push_back("127.0.0." + std::to_string(i + 1));
    }
}

// 开工前先算笔账：本地临时端口够不够用？不够的话跑到一半会冒出
// EADDRNOTAVAIL / EADDRINUSE，很多人以为是服务端崩了，其实是客户端自己没端口了。
void Bench::warnPortBudget() {
    int lo = 0;
    int hi = 0;
    FILE* f = std::fopen("/proc/sys/net/ipv4/ip_local_port_range", "r");
    if (f == nullptr) return;
    const int got = std::fscanf(f, "%d %d", &lo, &hi);
    std::fclose(f);
    if (got != 2 || hi <= lo) return;

    const long long per_ip = static_cast<long long>(hi) - lo + 1;
    const long long capacity = per_ip * static_cast<long long>(src_ips_.size());
    std::printf("[信息] 本地临时端口范围 %d~%d（每个源地址约 %s 个），本次用 %zu 个源地址，"
                "理论容量约 %s 条连接\n",
                lo, hi, withCommas(per_ip).c_str(), src_ips_.size(), withCommas(capacity).c_str());

    if (static_cast<long long>(opts_.conns) > capacity) {
        std::printf(
            "\n[警告] 你要开 %s 条连接，但本地端口容量只有约 %s 条，"
            "大概率会中途报「无法分配请求的地址 / 地址已被使用」。\n"
            "       建议（任选其一或组合）：\n"
            "       1) 多源地址：--source-ip-count 16  （回环目标时把连接摊到 127.0.0.1~127.0.0.16）\n"
            "       2) 扩大端口范围：sudo sysctl -w net.ipv4.ip_local_port_range=\"1024 65535\"\n"
            "       3) 复用 TIME_WAIT：sudo sysctl -w net.ipv4.tcp_tw_reuse=1\n"
            "       4) 减少连接数：--conns %lld\n\n",
            withCommas(opts_.conns).c_str(), withCommas(capacity).c_str(),
            std::min<long long>(static_cast<long long>(opts_.conns), capacity));
    }
}

// ---- 建连 ------------------------------------------------------------------

std::string Bench::buildHttpRequest(int conn_index) const {
    const std::string path = expandPlaceholder(opts_.path, conn_index);
    const std::string body = expandPlaceholder(opts_.body, conn_index);
    std::string req;
    req.reserve(256 + body.size());
    req += opts_.method;
    req += ' ';
    req += path;
    req += " HTTP/1.1\r\n";
    req += "Host: " + host_header_ + "\r\n";
    req += "User-Agent: mini-net-echo_bench/1.0\r\n";
    req += "Accept: */*\r\n";
    req += "Connection: keep-alive\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    return req;
}

bool Bench::startConnect(size_t index, std::string& err) {
    Conn& c = *conns_[index];

    // SOCK_NONBLOCK：connect 不会阻塞；SOCK_CLOEXEC：exec 后不把这个 fd 漏给子进程
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        err = std::string("创建 socket 失败：") + std::strerror(errno);
        return false;
    }

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (opts_.reuse_port) {
        // SO_REUSEPORT 让多个 socket（甚至多个压测进程）能绑同一个本地端口。
        // 但它并不能把「同一个四元组」变出多条连接，真正的扩容手段是上面的多源地址。
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    }
    if (opts_.nodelay) {
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    if (!c.src_ip.empty()) {
        sockaddr_in local;
        std::memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_port = 0;      // 0 = 让内核从临时端口范围里挑一个
        if (::inet_pton(AF_INET, c.src_ip.c_str(), &local.sin_addr) != 1) {
            err = "源地址格式不对：" + c.src_ip;
            ::close(fd);
            return false;
        }
        if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
            err = "绑定源地址 " + c.src_ip + " 失败：" + std::strerror(errno) +
                  "\n    （如果提示地址不可用，先确认它在不在本机：ip addr show lo）";
            ::close(fd);
            return false;
        }
    }

    c.fd = fd;
    c.req = http_mode_ ? buildHttpRequest(static_cast<int>(index)) : echo_payload_;
    c.last_progress = nowNs();

    const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&target_), target_len_);
    if (rc == 0) {
        c.state = ConnState::Waiting;       // 回环上偶尔会立刻连上，同样走起跑线
        return true;
    }
    if (errno == EINPROGRESS || errno == EINTR) {
        c.state = ConnState::Connecting;
        return true;
    }

    const int saved = errno;
    err = std::string("connect 失败（") + c.src_ip + " -> " + opts_.host + ":" +
          std::to_string(opts_.port) + "）：" + std::strerror(saved);
    ::close(fd);
    c.fd = -1;
    return false;
}

bool Bench::setupConns(std::string& err) {
    conns_.reserve(static_cast<size_t>(opts_.conns));
    for (int i = 0; i < opts_.conns; ++i) {
        std::unique_ptr<Conn> c(new Conn());
        c->src_ip = src_ips_[static_cast<size_t>(i) % src_ips_.size()];
        conns_.push_back(std::move(c));
    }

    for (size_t i = 0; i < conns_.size(); ++i) {
        std::string conn_err;
        if (!startConnect(i, conn_err)) {
            // 第 0 条就建不起来，说明是环境问题（地址不可用、权限不够），直接返回让上层报错
            if (i == 0) {
                err = conn_err;
                return false;
            }
            err_hist_["建连失败"] += 1;
            if (first_error_.empty()) first_error_ = conn_err;
            if (!duration_mode_) failed_ += opts_.requests;
            conns_[i]->state = ConnState::Failed;
            conns_[i]->fd = -1;
            ++conn_failed_;
            continue;
        }
        Conn& c = *conns_[i];
        ++alive_count_;
        // 只有还在建连的连接才计入"等起跑线"的计数。
        // 回环目标上 connect 有可能当场就返回成功（内核直接走完三次握手），
        // 这时候它已经是 Waiting 状态了，不能再算进 connecting_count_，
        // 否则计数永远归不了零，起跑线永远等不到。
        if (c.state == ConnState::Connecting) ++connecting_count_;

        epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        if (c.state == ConnState::Connecting) ev.events |= EPOLLOUT;
        ev.data.ptr = &c;
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, c.fd, &ev) < 0) {
            err = std::string("epoll_ctl(ADD) 失败：") + std::strerror(errno);
            return false;
        }
        c.ev_mask = ev.events;
    }
    return true;
}

// ---- epoll 事件注册 --------------------------------------------------------

void Bench::updateEvents(Conn& c) {
    if (c.fd < 0) return;
    uint32_t want = EPOLLIN;   // 永远关心可读
    // 需要写的情况：还在建连（要拿「连接完成」的通知），或者还有没发完的字节
    if (c.state == ConnState::Connecting || c.out_off < c.out.size()) want |= EPOLLOUT;
    if (want == c.ev_mask) return;      // 没变化就别麻烦内核了
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = want;
    ev.data.ptr = &c;
    if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, c.fd, &ev) < 0) {
        failConn(c, "epoll 操作失败", std::string("EPOLL_CTL_MOD: ") + std::strerror(errno));
        return;
    }
    c.ev_mask = want;
}

// ---- 收发 ------------------------------------------------------------------

bool Bench::moreRequests(const Conn& c) const {
    if (duration_mode_) return nowNs() < deadline_ns_;
    return c.sent_req < opts_.requests;
}

void Bench::refill(Conn& c) {
    if (c.state != ConnState::Ready) return;
    while (c.inflight < opts_.pipeline && moreRequests(c)) {
        if (c.ts_head == c.ts.size()) {     // 队列空了，复用这块内存
            c.ts.clear();
            c.ts_head = 0;
        }
        c.ts.push_back(nowNs());            // 记下发送时刻（严格说是「即将发出」的时刻）
        c.out.append(c.req);
        ++c.inflight;
        ++c.sent_req;
    }
}

// 把 out 里的字节尽量发出去。
// 返回 true 表示发干净了，false 表示还没发完（等 EPOLLOUT）或者已经出错。
bool Bench::flushOut(Conn& c) {
    while (c.out_off < c.out.size()) {
        const ssize_t n = ::send(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off, MSG_NOSIGNAL);
        if (n > 0) {
            c.out_off += static_cast<size_t>(n);
            c.last_progress = nowNs();
            total_bytes_out_ += static_cast<uint64_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            updateEvents(c);    // 让 epoll 开始关心可写
            return false;
        }
        failConn(c, "发送失败", std::string("send: ") + std::strerror(errno));
        return false;
    }
    c.out.clear();
    c.out_off = 0;
    updateEvents(c);
    return true;
}

void Bench::pump(Conn& c) {
    for (int i = 0; i < kWriteBudget; ++i) {
        if (c.state != ConnState::Ready) return;
        if (!flushOut(c)) return;       // 发不动了（或者已经出错）
        refill(c);                      // 补上新的在途请求
        if (c.out.empty()) return;      // 没有新东西可发了
    }
}

// 校验一段回显数据。payload 是循环使用的固定内容，所以全局第 k 个字节应该等于
// payload[k % size]。分两段比较，避免逐字节循环（压测工具自己不能太慢）。
bool Bench::verifyChunk(uint64_t base, const char* buf, size_t n) const {
    const size_t sz = echo_payload_.size();
    if (sz == 0 || n == 0) return true;
    const size_t start = static_cast<size_t>(base % sz);
    const size_t first = std::min(n, sz - start);
    if (std::memcmp(buf, echo_payload_.data() + start, first) != 0) return false;
    if (n > first && std::memcmp(buf + first, echo_payload_.data(), n - first) != 0) return false;
    return true;
}

// 从 in 缓冲里解析出所有完整的 HTTP 响应，每解析出一个就记一次延迟。
bool Bench::drainHttp(Conn& c) {
    for (;;) {
        int status = 0;
        std::string perr;
        const ParseResult r = parseOne(c, status, perr);
        if (r == ParseResult::NeedMore) break;
        if (r == ParseResult::Error) {
            failConn(c, "响应解析失败", perr);
            return false;
        }
        onResponseComplete(c, status);
    }
    // 回收已经消费掉的前缀，否则缓冲区会越用越大
    if (c.in_off == c.in.size()) {
        c.in.clear();
        c.in_off = 0;
    } else if (c.in_off > 8192) {
        c.in.erase(0, c.in_off);
        c.in_off = 0;
    }
    return true;
}

// 解析一个完整响应：成功时把状态码写进 status，并把 in_off 推到响应末尾。
//
// 说明：这里每次都从 in_off 重新找响应头，所以如果 body 很大、分很多次才收全，
// 响应头会被重复解析几遍。压测场景下响应通常只有几十到几百字节，完全够用，
// 换来的是状态机简单、不容易出边界 bug。
ParseResult Bench::parseOne(Conn& c, int& status, std::string& err) {
    const size_t avail = c.in.size() - c.in_off;
    if (avail < 16) return ParseResult::NeedMore;    // 连一个状态行都凑不齐

    const size_t hdr_end = c.in.find("\r\n\r\n", c.in_off);
    if (hdr_end == std::string::npos) {
        if (avail > kMaxHeaderBytes) {
            err = "响应头超过 64 KiB 还没结束，服务端可能没有按 HTTP 格式回应";
            return ParseResult::Error;
        }
        return ParseResult::NeedMore;
    }
    const size_t body_start = hdr_end + 4;

    // 状态行：HTTP/1.1 200 OK
    const size_t line_end = c.in.find("\r\n", c.in_off);
    if (line_end == std::string::npos || line_end <= c.in_off ||
        c.in.compare(c.in_off, 5, "HTTP/") != 0) {
        err = "状态行不是 HTTP 格式（可能连到的不是 HTTP 服务）";
        return ParseResult::Error;
    }
    const size_t sp = c.in.find(' ', c.in_off);
    if (sp == std::string::npos || sp >= line_end) {
        err = "状态行里找不到状态码";
        return ParseResult::Error;
    }
    status = std::atoi(c.in.c_str() + sp + 1);
    if (status < 100 || status > 599) {
        err = "状态码不合法：解析出 " + std::to_string(status);
        return ParseResult::Error;
    }

    // 逐行扫响应头，只关心 Content-Length 和 Transfer-Encoding
    long long content_length = -1;
    bool chunked = false;
    size_t p = line_end + 2;
    while (p < hdr_end) {
        const size_t e = c.in.find("\r\n", p);
        if (e == std::string::npos || e > hdr_end) break;
        const size_t colon = c.in.find(':', p);
        if (colon != std::string::npos && colon < e) {
            const std::string key = toLower(c.in.substr(p, colon - p));
            const std::string val = trim(c.in.substr(colon + 1, e - colon - 1));
            if (key == "content-length") {
                content_length = std::atoll(val.c_str());
            } else if (key == "transfer-encoding" &&
                       toLower(val).find("chunked") != std::string::npos) {
                chunked = true;
            }
        }
        p = e + 2;
    }

    if (chunked) {
        err = "响应用了 chunked 编码，本压测客户端不支持；接口契约要求所有响应都带 "
              "Content-Length，请检查服务端";
        return ParseResult::Error;
    }
    if (content_length < 0) {
        err = "响应缺少 Content-Length，无法判断响应到哪里结束（契约要求所有响应都带它）";
        return ParseResult::Error;
    }
    const size_t need = static_cast<size_t>(content_length);
    if (c.in.size() - body_start < need) return ParseResult::NeedMore;   // body 还没收全

    c.in_off = body_start + need;
    return ParseResult::Complete;
}

void Bench::onResponseComplete(Conn& c, int status) {
    uint64_t lat = 0;
    if (c.ts_head < c.ts.size()) {
        const uint64_t sent_at = c.ts[c.ts_head++];
        lat = nowNs() - sent_at;
        if (c.ts_head == c.ts.size()) {     // 队列用完了就清空，下次复用这块内存
            c.ts.clear();
            c.ts_head = 0;
        }
    }
    if (c.inflight > 0) --c.inflight;
    ++c.done_req;
    c.last_progress = nowNs();

    if (http_mode_) {
        status_hist_[status] += 1;
        bool expected = false;
        for (size_t i = 0; i < opts_.expect_status.size(); ++i) {
            if (opts_.expect_status[i] == status) {
                expected = true;
                break;
            }
        }
        if (expected) {
            ++ok_;
        } else {
            ++bad_status_;
        }
    } else {
        ++ok_;
    }

    if (lat > 0) {
        if (lat_ns_.size() < kMaxLatencySamples) {
            lat_ns_.push_back(lat);
        } else {
            ++lat_dropped_;
        }
    }
}

bool Bench::doRead(Conn& c) {
    char buf[65536];
    for (int i = 0; i < kReadBudget; ++i) {
        const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            const size_t got = static_cast<size_t>(n);
            c.last_progress = nowNs();
            total_bytes_in_ += static_cast<uint64_t>(got);

            if (http_mode_) {
                c.in.append(buf, got);
                if (!drainHttp(c)) return false;
            } else {
                if (opts_.verify && !verifyChunk(c.echo_recv, buf, got)) {
                    failConn(c, "回显内容不对",
                             "收到的字节和发出去的不一样（第 " + std::to_string(c.echo_recv) +
                             " 字节附近开始不一致），服务端回显逻辑可能有 bug");
                    return false;
                }
                c.echo_recv += static_cast<uint64_t>(got);
                c.echo_partial += static_cast<uint64_t>(got);
                // 回显模式下每条响应的长度就等于请求长度，收满一条就算完成一个请求
                const uint64_t one = static_cast<uint64_t>(opts_.size);
                while (c.echo_partial >= one) {
                    c.echo_partial -= one;
                    onResponseComplete(c, 0);
                }
            }
            continue;   // 继续读，一直读到 EAGAIN（LT 模式下这样能少进几次 epoll_wait）
        }

        if (n == 0) {
            // 对端关连接了。还有在途请求没收到响应，就是被提前掐断了。
            if (c.inflight > 0 || moreRequests(c)) {
                failConn(c, "对端提前关闭",
                         "还有 " + std::to_string(c.inflight) +
                             " 个在途请求没收到响应，服务端就把连接关了"
                             "（可能是 keep-alive 没实现，或者空闲超时设得太短）");
            } else {
                closeConn(c, true);
            }
            return false;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        failConn(c, "接收失败", std::string("recv: ") + std::strerror(errno));
        return false;
    }
    return true;
}

// ---- 事件处理 --------------------------------------------------------------

void Bench::onEvent(Conn& c, uint32_t events) {
    // 同一批事件里可能夹着已经被关掉的连接（比如刚才的周期扫描把它判超时了），先查存活
    if (!alive(c)) return;

    if (c.state == ConnState::Connecting) {
        int so_err = 0;
        socklen_t len = sizeof(so_err);
        if (::getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &so_err, &len) < 0) {
            failConn(c, "建连失败", std::string("getsockopt(SO_ERROR): ") + std::strerror(errno));
            return;
        }
        if (so_err != 0) {
            const std::string detail = std::string("connect 返回错误：") + std::strerror(so_err);
            if (so_err == EADDRNOTAVAIL || so_err == EADDRINUSE || so_err == EACCES) {
                failConn(c, "本地端口耗尽",
                         detail + "（客户端没端口可用了：用 --source-ip-count 或者按启动时的提示调大端口范围）");
            } else if (so_err == ECONNREFUSED) {
                failConn(c, "建连被拒绝", detail + "（服务端没在监听？）");
            } else {
                failConn(c, "建连失败", detail);
            }
            return;
        }
        if (connecting_count_ > 0) --connecting_count_;
        c.last_progress = nowNs();
        if (started_) {
            // 起跑线已经过了（比如这条连接建得特别慢），直接进入工作状态
            c.state = ConnState::Ready;
            pump(c);
        } else {
            c.state = ConnState::Waiting;
        }
        updateEvents(c);
        return;
    }

    if (events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
        if (!doRead(c)) return;
    }
    if (!alive(c)) return;

    // 不管这次事件是不是可写，都要调一次 pump：
    // 收到响应后 inflight 会降到 0，得马上补发下一个请求；而这时候 out 是空的，
    // epoll 里根本没注册 EPOLLOUT，光等可写事件会一直等不到（连接就"哑"了，
    // 只能等读超时才报错）。pump 内部会先 flush 再 refill，没活干时开销极小。
    pump(c);
    if (!alive(c)) return;
    maybeFinish(c);
}

void Bench::maybeFinish(Conn& c) {
    if (c.state != ConnState::Ready) return;
    if (moreRequests(c)) return;                  // 还有请求要发
    if (c.inflight > 0) return;                   // 还有响应没收
    if (c.out_off < c.out.size()) return;         // 还有字节没发出去
    closeConn(c, true);
}

void Bench::closeConn(Conn& c, bool success) {
    if (c.fd >= 0) {
        ::epoll_ctl(epfd_, EPOLL_CTL_DEL, c.fd, nullptr);
        ::close(c.fd);
        c.fd = -1;
    }
    c.state = success ? ConnState::Done : ConnState::Failed;
    if (alive_count_ > 0) --alive_count_;
}

void Bench::failConn(Conn& c, const std::string& category, const std::string& detail) {
    if (c.state == ConnState::Failed || c.state == ConnState::Done) return;

    if (c.state == ConnState::Connecting && connecting_count_ > 0) --connecting_count_;

    // 这条连接上「本该完成但没完成」的请求数，全部计入失败
    long lost = 0;
    if (duration_mode_) {
        lost = c.inflight;                        // 定压模式下只算在途的那些
    } else {
        lost = opts_.requests - c.done_req;
    }
    if (lost > 0) failed_ += lost;

    ++conn_failed_;
    err_hist_[category] += 1;
    if (first_error_.empty()) {
        first_error_ = category + (detail.empty() ? std::string() : "：" + detail);
    }
    closeConn(c, false);
}

// ---- 起跑线与周期扫描 ------------------------------------------------------

void Bench::maybeStart() {
    if (started_) return;
    if (connecting_count_ > 0) return;      // 还有连接没建好，再等等
    started_ = true;
    t0_ns_ = nowNs();
    if (duration_mode_) deadline_ns_ = t0_ns_ + static_cast<uint64_t>(opts_.duration * 1e9);
    for (size_t i = 0; i < conns_.size(); ++i) {
        Conn& c = *conns_[i];
        if (c.state == ConnState::Waiting) {
            c.state = ConnState::Ready;
            c.last_progress = t0_ns_;
            pump(c);                        // 开跑
        }
    }
    for (size_t i = 0; i < conns_.size(); ++i) maybeFinish(*conns_[i]);
}

void Bench::checkFinishes() {
    for (size_t i = 0; i < conns_.size(); ++i) {
        Conn& c = *conns_[i];
        if (c.state == ConnState::Ready) maybeFinish(c);
    }
}

void Bench::checkTimeouts() {
    const uint64_t now = nowNs();
    for (size_t i = 0; i < conns_.size(); ++i) {
        Conn& c = *conns_[i];
        if (!alive(c) || c.state == ConnState::Waiting) continue;
        const uint64_t limit = (c.state == ConnState::Connecting) ? connect_timeout_ns_ : timeout_ns_;
        if (now - c.last_progress <= limit) continue;

        if (c.state == ConnState::Connecting) {
            int so_err = 0;
            socklen_t len = sizeof(so_err);
            std::string detail = "连接建立超时（" + std::to_string(opts_.connect_timeout_ms) + " ms）";
            if (::getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &so_err, &len) == 0 && so_err != 0) {
                detail += std::string("，SO_ERROR=") + std::strerror(so_err);
            }
            failConn(c, "建连超时", detail);
        } else {
            failConn(c, "读超时",
                     "超过 " + std::to_string(opts_.timeout_ms) + " ms 没有任何进展（在途请求 " +
                         std::to_string(c.inflight) + " 个）");
        }
    }
}

void Bench::periodicScan() {
    checkFinishes();    // 先收尾：该结束的连接正常结束掉
    checkTimeouts();    // 再判超时，避免刚跑完的连接被误判成超时
}

// ---- 主流程 ----------------------------------------------------------------

int Bench::run() {
    ::signal(SIGPIPE, SIG_IGN);     // 对端关了还写会收到 SIGPIPE，不忽略的话进程直接死

    http_mode_ = (opts_.mode == "http");
    duration_mode_ = (opts_.duration > 0.0);
    timeout_ns_ = static_cast<uint64_t>(opts_.timeout_ms) * 1000000ull;
    connect_timeout_ns_ = static_cast<uint64_t>(opts_.connect_timeout_ms) * 1000000ull;
    std::memset(&target_, 0, sizeof(target_));
    std::memset(&ru0_, 0, sizeof(ru0_));
    std::memset(&ru1_, 0, sizeof(ru1_));

    if (!http_mode_) echo_payload_ = makeEchoPayload(opts_.size);

    std::string err;

    // 1) 解析目标地址
    if (!resolveTarget(err)) {
        std::fprintf(stderr, "[错误] %s\n", err.c_str());
        return 2;
    }

    // 2) 连之前先探一下路，服务端没起来的话马上给出人话提示
    if (!preflight(err)) {
        std::fprintf(stderr, "[错误] %s\n", err.c_str());
        return 2;
    }

    // 3) 源地址与端口预算
    std::string warn;
    prepareSourceIps(warn);
    if (!warn.empty()) std::fprintf(stderr, "[警告] %s\n", warn.c_str());
    warnPortBudget();

    // 4) epoll 与连接
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        std::fprintf(stderr, "[错误] epoll_create1 失败：%s\n", std::strerror(errno));
        return 2;
    }
    events_.resize(kMaxEvents);

    std::printf("[信息] 开始建立 %s 条连接（%s -> %s:%d，%zu 个源地址）……\n",
                withCommas(opts_.conns).c_str(), http_mode_ ? "HTTP" : "回显协议",
                opts_.host.c_str(), opts_.port, src_ips_.size());
    std::fflush(stdout);

    setup_start_ns_ = nowNs();
    if (!setupConns(err)) {
        std::fprintf(stderr, "[错误] %s\n", err.c_str());
        ::close(epfd_);
        return 2;
    }

    // 5) 事件循环
    ::getrusage(RUSAGE_SELF, &ru0_);
    last_scan_ns_ = nowNs();
    maybeStart();   // 回环上所有连接可能在建的时候就绪了，这里先开一次跑，别白等 100 ms

    while (alive_count_ > 0) {
        const int n = ::epoll_wait(epfd_, events_.data(), kMaxEvents, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "[错误] epoll_wait 失败：%s\n", std::strerror(errno));
            break;
        }
        // 注意：整个压测过程中不会再创建新 fd（连接都是开跑前一次性建好的），
        // 所以不存在「fd 号被复用导致事件张冠李戴」的问题。
        for (int i = 0; i < n; ++i) {
            Conn* c = static_cast<Conn*>(events_[i].data.ptr);
            if (c != nullptr) onEvent(*c, events_[i].events);
        }

        maybeStart();       // 所有连接都建好了就一起开跑

        const uint64_t now = nowNs();
        if (now - last_scan_ns_ >= kScanIntervalNs) {
            last_scan_ns_ = now;
            periodicScan();
        }
    }
    t1_ns_ = nowNs();
    ::getrusage(RUSAGE_SELF, &ru1_);

    // 6) 出报告
    report();
    if (!opts_.json_out.empty()) writeJson();

    ::close(epfd_);

    if (ok_ == 0 && failed_ > 0) return 2;      // 一个都没成功，基本是环境问题
    return (failed_ > 0 || bad_status_ > 0) ? 3 : 0;
}

// ---- 报告 ------------------------------------------------------------------

namespace {
// 用「最近秩」法算分位数，和 scripts/bench_quick.py 里的算法保持一致，
// 这样新旧两套工具的数字可以直接对比。
double percentileUs(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return -1.0;
    size_t idx = static_cast<size_t>(static_cast<double>(sorted.size()) * p / 100.0);
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return static_cast<double>(sorted[idx]) / 1000.0;   // 纳秒 -> 微秒
}
}  // namespace

void Bench::report() {
    const double elapsed = (t1_ns_ > t0_ns_ && t0_ns_ > 0) ? static_cast<double>(t1_ns_ - t0_ns_) / 1e9 : 0.0;
    const double setup_ms = (t0_ns_ > setup_start_ns_)
                                ? static_cast<double>(t0_ns_ - setup_start_ns_) / 1e6
                                : 0.0;
    const long long done_total = ok_ + bad_status_;
    const long long expected = duration_mode_ ? 0 : static_cast<long long>(opts_.conns) * opts_.requests;

    std::vector<uint64_t> lat = lat_ns_;
    std::sort(lat.begin(), lat.end());

    double sum_lat = 0.0;
    for (size_t i = 0; i < lat.size(); ++i) sum_lat += static_cast<double>(lat[i]);
    const double avg_us = lat.empty() ? -1.0 : (sum_lat / static_cast<double>(lat.size()) / 1000.0);

    const double cpu_user = static_cast<double>(ru1_.ru_utime.tv_sec - ru0_.ru_utime.tv_sec) +
                            static_cast<double>(ru1_.ru_utime.tv_usec - ru0_.ru_utime.tv_usec) / 1e6;
    const double cpu_sys = static_cast<double>(ru1_.ru_stime.tv_sec - ru0_.ru_stime.tv_sec) +
                           static_cast<double>(ru1_.ru_stime.tv_usec - ru0_.ru_stime.tv_usec) / 1e6;
    const double cpu_total = cpu_user + cpu_sys;
    const double cpu_pct = (elapsed > 0.0) ? (cpu_total / elapsed * 100.0) : 0.0;
    const double peak_rss_mb = static_cast<double>(ru1_.ru_maxrss) / 1024.0;   // Linux 上单位是 KB

    std::string per_conn;
    if (duration_mode_) {
        per_conn = "跑满 " + std::to_string(opts_.duration) + " 秒";
    } else {
        per_conn = withCommas(opts_.requests);
    }
    std::string src_desc = "由内核分配";
    if (!src_ips_.empty() && !src_ips_[0].empty()) src_desc = src_ips_[0] + " 起";

    std::printf("\n");
    std::printf("================================================================\n");
    std::printf(" mini-net 压测结果");
    if (!opts_.label.empty()) std::printf("   [%s]", opts_.label.c_str());
    std::printf("\n");
    std::printf("================================================================\n");
    std::printf(" 模式              : %s\n", http_mode_ ? "HTTP" : "回显协议（echo）");
    std::printf(" 目标              : %s:%d\n", opts_.host.c_str(), opts_.port);
    if (http_mode_) {
        std::printf(" 请求              : %s %s\n", opts_.method.c_str(), opts_.path.c_str());
        std::printf(" 期望状态码        : ");
        for (size_t i = 0; i < opts_.expect_status.size(); ++i) {
            std::printf("%s%d", i == 0 ? "" : ",", opts_.expect_status[i]);
        }
        std::printf("\n");
    } else {
        std::printf(" 消息大小          : %d 字节\n", opts_.size);
    }
    std::printf(" 并发连接          : %s，每条连接 %s 个请求，pipeline=%d\n",
                withCommas(opts_.conns).c_str(), per_conn.c_str(), opts_.pipeline);
    std::printf(" 建立连接耗时      : %.1f ms（所有连接都就绪了才开始计时）\n", setup_ms);
    if (!duration_mode_) {
        std::printf(" 期望请求数        : %s\n", withCommas(expected).c_str());
    }
    std::printf(" 收到响应          : %s\n", withCommas(done_total).c_str());
    std::printf(" 成功              : %s\n", withCommas(ok_).c_str());
    if (bad_status_ > 0) {
        std::printf(" 状态码不符预期    : %s\n", withCommas(bad_status_).c_str());
    }
    std::printf(" 失败请求          : %s", withCommas(failed_).c_str());
    if (conn_failed_ > 0) std::printf("（%s 条连接出错）", withCommas(conn_failed_).c_str());
    std::printf("\n");
    std::printf(" 压测耗时          : %.3f s\n", elapsed);
    if (elapsed > 0) {
        std::printf(" QPS（成功）       : %s\n",
                    withCommas(static_cast<long long>(static_cast<double>(ok_) / elapsed)).c_str());
        std::printf(" QPS（总完成）     : %s\n",
                    withCommas(static_cast<long long>(static_cast<double>(done_total) / elapsed)).c_str());
    } else {
        std::printf(" QPS               : 没有产生有效负载\n");
    }
    if (!lat.empty()) {
        std::printf(" 延迟 P50/P90/P99/P999 (微秒): %.0f / %.0f / %.0f / %.0f\n",
                    percentileUs(lat, 50), percentileUs(lat, 90), percentileUs(lat, 99),
                    percentileUs(lat, 99.9));
        std::printf(" 延迟 最小/平均/最大 (微秒) : %.0f / %.1f / %.0f\n",
                    static_cast<double>(lat.front()) / 1000.0, avg_us,
                    static_cast<double>(lat.back()) / 1000.0);
        if (lat_dropped_ > 0) {
            std::printf("                             （另有 %s 条超出样本上限，没参与统计）\n",
                        withCommas(lat_dropped_).c_str());
        }
    } else {
        std::printf(" 延迟              : 没有采集到样本\n");
    }
    std::printf(" 进程 CPU 时间     : 用户 %.2f s + 系统 %.2f s = %.2f s（占单核 %.0f%%）\n",
                cpu_user, cpu_sys, cpu_total, cpu_pct);
    std::printf(" 峰值内存 RSS      : %.1f MB\n", peak_rss_mb);
    std::printf(" 客户端源地址      : %zu 个（%s）\n", src_ips_.size(), src_desc.c_str());
    std::printf(" 收发字节          : 收 %.2f MB / 发 %.2f MB\n",
                static_cast<double>(total_bytes_in_) / 1048576.0,
                static_cast<double>(total_bytes_out_) / 1048576.0);

    if (http_mode_ && !status_hist_.empty()) {
        std::printf(" 状态码分布        : ");
        for (std::map<int, long long>::const_iterator it = status_hist_.begin();
             it != status_hist_.end(); ++it) {
            if (it != status_hist_.begin()) std::printf("，");
            std::printf("%d 共 %s 次", it->first, withCommas(it->second).c_str());
        }
        std::printf("\n");
    }
    if (!err_hist_.empty()) {
        std::printf(" 错误分类          : ");
        for (std::map<std::string, long long>::const_iterator it = err_hist_.begin();
             it != err_hist_.end(); ++it) {
            if (it != err_hist_.begin()) std::printf("，");
            std::printf("%s 共 %s 条连接", it->first.c_str(), withCommas(it->second).c_str());
        }
        std::printf("\n");
    }
    if (!first_error_.empty()) {
        std::printf(" 首个错误详情      : %s\n", first_error_.c_str());
    }

    std::printf("----------------------------------------------------------------\n");
    // 几条「这个数字能不能信」的自检提示
    if (t0_ns_ == 0 || (done_total == 0 && failed_ > 0)) {
        std::printf(" [注意] 一条连接都没能正常工作，服务端可能根本没起来，或者参数不对。\n");
    }
    if (cpu_pct > 85.0) {
        std::printf(" [注意] 压测客户端自己已经吃掉单核 CPU 的 %.0f%%，它很可能就是瓶颈，\n"
                    "        这个 QPS 反映的是客户端的上限，不是服务端的上限。\n"
                    "        想让客户端更强：多开几个进程（每个进程用不同的源地址和连接数），\n"
                    "        或者换一台机器专门跑压测。\n",
                    cpu_pct);
    }
    if (lat_dropped_ > 0) {
        std::printf(" [注意] 延迟样本被截断了，P999 这类长尾数字仅供参考。\n");
    }
    if (failed_ > 0) {
        std::printf(" [注意] 有 %s 个请求没拿到响应，上面的 QPS 只是成功部分的吞吐，\n"
                    "        别把它当成服务端上限。\n",
                    withCommas(failed_).c_str());
    }
    std::printf(" 说明：本工具和服务端可能跑在同一台机器上（同机压测），两边抢 CPU、抢内存带宽，\n"
                "      测出来的 QPS 会偏低、P99 会偏高。要做正式对比请用两台机器，\n"
                "      或者至少记录下客户端 CPU 占用，确认瓶颈确实在服务端。\n");
    std::printf("================================================================\n");
    std::fflush(stdout);
}

void Bench::writeJson() {
    FILE* f = std::fopen(opts_.json_out.c_str(), "w");
    if (f == nullptr) {
        std::fprintf(stderr, "[警告] 写不了 JSON 文件 %s：%s\n", opts_.json_out.c_str(),
                     std::strerror(errno));
        return;
    }
    const double elapsed = (t1_ns_ > t0_ns_ && t0_ns_ > 0) ? static_cast<double>(t1_ns_ - t0_ns_) / 1e9 : 0.0;
    std::vector<uint64_t> lat = lat_ns_;
    std::sort(lat.begin(), lat.end());
    const double cpu_user = static_cast<double>(ru1_.ru_utime.tv_sec - ru0_.ru_utime.tv_sec) +
                            static_cast<double>(ru1_.ru_utime.tv_usec - ru0_.ru_utime.tv_usec) / 1e6;
    const double cpu_sys = static_cast<double>(ru1_.ru_stime.tv_sec - ru0_.ru_stime.tv_sec) +
                           static_cast<double>(ru1_.ru_stime.tv_usec - ru0_.ru_stime.tv_usec) / 1e6;
    const long long done_total = ok_ + bad_status_;

    // label 里可能有引号或反斜杠，简单转义一下
    std::string label;
    for (size_t i = 0; i < opts_.label.size(); ++i) {
        const char ch = opts_.label[i];
        if (ch == '"' || ch == '\\') label.push_back('\\');
        label.push_back(ch);
    }

    std::fprintf(f,
                 "{\n"
                 "  \"label\": \"%s\",\n"
                 "  \"mode\": \"%s\",\n"
                 "  \"host\": \"%s\",\n"
                 "  \"port\": %d,\n"
                 "  \"conns\": %d,\n"
                 "  \"requests_per_conn\": %ld,\n"
                 "  \"size\": %d,\n"
                 "  \"pipeline\": %d,\n"
                 "  \"duration_s\": %.3f,\n"
                 "  \"elapsed_s\": %.6f,\n"
                 "  \"completed\": %lld,\n"
                 "  \"ok\": %lld,\n"
                 "  \"bad_status\": %lld,\n"
                 "  \"failed\": %lld,\n"
                 "  \"failed_conns\": %lld,\n"
                 "  \"qps_ok\": %.2f,\n"
                 "  \"qps_total\": %.2f,\n"
                 "  \"lat_p50_us\": %.2f,\n"
                 "  \"lat_p90_us\": %.2f,\n"
                 "  \"lat_p99_us\": %.2f,\n"
                 "  \"lat_p999_us\": %.2f,\n"
                 "  \"lat_max_us\": %.2f,\n"
                 "  \"cpu_user_s\": %.3f,\n"
                 "  \"cpu_sys_s\": %.3f,\n"
                 "  \"cpu_percent\": %.1f,\n"
                 "  \"peak_rss_mb\": %.1f,\n"
                 "  \"bytes_in\": %llu,\n"
                 "  \"bytes_out\": %llu,\n"
                 "  \"source_ips\": %zu\n"
                 "}\n",
                 label.c_str(), opts_.mode.c_str(), opts_.host.c_str(), opts_.port, opts_.conns,
                 opts_.requests, opts_.size, opts_.pipeline, opts_.duration, elapsed, done_total,
                 ok_, bad_status_, failed_, conn_failed_,
                 elapsed > 0 ? static_cast<double>(ok_) / elapsed : 0.0,
                 elapsed > 0 ? static_cast<double>(done_total) / elapsed : 0.0,
                 percentileUs(lat, 50), percentileUs(lat, 90), percentileUs(lat, 99),
                 percentileUs(lat, 99.9),
                 lat.empty() ? 0.0 : static_cast<double>(lat.back()) / 1000.0, cpu_user, cpu_sys,
                 elapsed > 0 ? (cpu_user + cpu_sys) / elapsed * 100.0 : 0.0,
                 static_cast<double>(ru1_.ru_maxrss) / 1024.0,
                 static_cast<unsigned long long>(total_bytes_in_),
                 static_cast<unsigned long long>(total_bytes_out_), src_ips_.size());
    std::fclose(f);
    std::printf("[信息] 结果已写入 %s\n", opts_.json_out.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    std::string err;
    if (!parseArgs(argc, argv, opts, err)) {
        std::fprintf(stderr, "[错误] %s\n\n", err.c_str());
        printUsage(argv[0]);
        return 2;
    }
    Bench bench(opts);
    return bench.run();
}
