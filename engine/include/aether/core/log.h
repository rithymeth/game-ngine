#pragma once

#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace aether {

enum class LogLevel : int { Trace = 0, Info, Warn, Error, Fatal };

const char* LogLevelName(LogLevel level);

struct LogLine {
    LogLevel level = LogLevel::Info;
    std::string category;
    std::string message;
    double time = 0.0; // seconds since the logger started
};

class Logger {
public:
    static Logger& Instance();

    void SetMinLevel(LogLevel level) { min_level_ = level; }
    LogLevel MinLevel() const { return min_level_; }
    void Log(LogLevel level, std::string_view category, std::string_view message);

    // Listeners (Phase 23: the console, crash reports): called for every
    // line that passes the level, on the logging thread, outside the
    // logger's lock (so a sink may log). Returns an id for RemoveSink.
    using Sink = std::function<void(const LogLine&)>;
    int AddSink(Sink sink);
    void RemoveSink(int id);
    // Whether lines also go to stdout/stderr (default true).
    void SetStdout(bool enabled) { stdout_ = enabled; }

    // The most recent lines, oldest first (up to RecentCapacity()).
    std::vector<LogLine> Recent() const;
    static constexpr size_t RecentCapacity() { return 512; }
    void ClearRecent();

private:
    LogLevel min_level_ = LogLevel::Trace;
    bool stdout_ = true;
    mutable std::mutex mutex_;
    std::deque<LogLine> recent_;
    std::vector<std::pair<int, std::shared_ptr<Sink>>> sinks_;
    int next_sink_ = 1;
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
