#pragma once

#include <cstdio>
#include <mutex>
#include <string_view>

namespace aether {

enum class LogLevel : int { Trace = 0, Info, Warn, Error, Fatal };

class Logger {
public:
    static Logger& Instance();

    void SetMinLevel(LogLevel level) { min_level_ = level; }
    void Log(LogLevel level, std::string_view category, std::string_view message);

private:
    LogLevel min_level_ = LogLevel::Trace;
    std::mutex mutex_;
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
