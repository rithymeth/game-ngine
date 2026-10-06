#include "aether/core/log.h"

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cstdio>

namespace aether {

namespace {

const char* LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

} // namespace

Logger::Logger(usize buffer_size) : buffer_size_(buffer_size > 0 ? buffer_size : 1) {}

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::Log(LogLevel level, std::string_view category, std::string_view message) {
    if (level < min_level_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    FILE* stream = (level >= LogLevel::Error) ? stderr : stdout;
    std::fprintf(stream, "[%s] %.*s: %.*s\n", LevelToString(level), static_cast<int>(category.size()),
                 category.data(), static_cast<int>(message.size()), message.data());
    std::fflush(stream);

    // Keep a copy in the recent-log ring buffer (used by the crash handler).
    ring_buffer_.emplace_back(LogEntry{level, std::string(category), std::string(message)});
    if (ring_buffer_.size() > buffer_size_) {
        ring_buffer_.pop_front();
    }
}

std::vector<LogEntry> Logger::RecentLogLines(usize max) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const usize take = std::min(max, ring_buffer_.size());
    if (take == 0) {
        return {};
    }
    return std::vector<LogEntry>(ring_buffer_.begin(), ring_buffer_.begin() + static_cast<std::ptrdiff_t>(take));
}

void Logger::SetLogBufferSize(usize n) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_size_ = n > 0 ? n : 1;
    while (ring_buffer_.size() > buffer_size_) {
        ring_buffer_.pop_front();
    }
}

void LogFormatted(LogLevel level, std::string_view category, const char* fmt, ...) {
    char buffer[1024];

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    Logger::Instance().Log(level, category, buffer);
}

} // namespace aether
