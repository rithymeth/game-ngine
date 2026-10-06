#pragma once

#include "aether/core/base.h"

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace aether {

enum class LogLevel : int { Trace = 0, Info, Warn, Error, Fatal };

struct LogEntry {
    LogLevel level = LogLevel::Trace;
    std::string category;
    std::string message;
};

class Logger {
public:
    static Logger& Instance();

    void SetMinLevel(LogLevel level) { min_level_ = level; }
    void Log(LogLevel level, std::string_view category, std::string_view message);

    // Thread-safe ring buffer of the most recent log lines, oldest -> newest.
    std::vector<LogEntry> RecentLogLines(usize max = 200) const;
    // Resizes the kept history (default 200). Truncates if smaller.
    void SetLogBufferSize(usize n);

private:
    explicit Logger(usize buffer_size = 200);

    LogLevel min_level_ = LogLevel::Trace;
    mutable std::mutex mutex_;
    usize buffer_size_;
    std::deque<LogEntry> ring_buffer_;
};

void LogFormatted(LogLevel level, std::string_view category, const char* fmt, ...);

} // namespace aether

#define AETHER_LOG_TRACE(category, fmt, ...)                                                     \
    ::aether::LogFormatted(::aether::LogLevel::Trace, category, fmt, ##__VA_ARGS__)
#define AETHER_LOG_INFO(category, fmt, ...)                                                       \
    ::aether::LogFormatted(::aether::LogLevel::Info, category, fmt, ##__VA_ARGS__)
#define AETHER_LOG_WARN(category, fmt, ...)                                                       \
    ::aether::LogFormatted(::aether::LogLevel::Warn, category, fmt, ##__VA_ARGS__)
#define AETHER_LOG_ERROR(category, fmt, ...)                                                      \
    ::aether::LogFormatted(::aether::LogLevel::Error, category, fmt, ##__VA_ARGS__)
#define AETHER_LOG_FATAL(category, fmt, ...)                                                      \
    ::aether::LogFormatted(::aether::LogLevel::Fatal, category, fmt, ##__VA_ARGS__)
