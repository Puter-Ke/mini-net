#pragma once
// 极简日志：级别过滤 + 时间戳 + 文件行号
// 面试点：为什么不用 std::cout？—— 多线程下 cout 每行一次锁且可能被交错，
//        生产级日志还要做异步落盘（双缓冲 + 后台线程），那是 M4 之后的加分项。
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace mininet {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

inline LogLevel& logLevel() {
    static LogLevel lv = LogLevel::Info;
    return lv;
}
inline void setLogLevel(LogLevel lv) { logLevel() = lv; }

inline const char* levelName(LogLevel lv) {
    switch (lv) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        default:              return "ERROR";
    }
}

inline void logMessage(LogLevel lv, const char* file, int line, const char* fmt, ...) {
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);

    char ts[32];
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    std::fprintf(stderr, "[%s] [%s] %s:%d  %s\n", ts, levelName(lv), file, line, body);
}

}  // namespace mininet

#define MN_LOG(level, ...)                                                       \
    do {                                                                         \
        if ((level) >= ::mininet::logLevel())                                    \
            ::mininet::logMessage((level), __FILE__, __LINE__, __VA_ARGS__);     \
    } while (0)

#define LOG_DEBUG(...) MN_LOG(::mininet::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)  MN_LOG(::mininet::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...)  MN_LOG(::mininet::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) MN_LOG(::mininet::LogLevel::Error, __VA_ARGS__)
