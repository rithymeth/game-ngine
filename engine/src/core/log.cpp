#include "aether/core/log.h"

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
