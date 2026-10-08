#include "aether/core/log.h"

#include <cstdarg>
#include <chrono>
#include <cstdio>

namespace aether {

namespace {

const auto kStart = std::chrono::steady_clock::now();

} // namespace

const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::Log(LogLevel level, std::string_view category, std::string_view message) {
    if (level < min_level_.load(std::memory_order_relaxed)) {
        return;
    }
    LogLine line{level, std::string(category), std::string(message),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - kStart).count()};
    std::vector<std::shared_ptr<Sink>> sinks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stdout_.load(std::memory_order_relaxed)) {
            FILE* stream = (level >= LogLevel::Error) ? stderr : stdout;
            std::fprintf(stream, "[%s] %.*s: %.*s\n", LogLevelName(level), static_cast<int>(category.size()),
                         category.data(), static_cast<int>(message.size()), message.data());
            std::fflush(stream);
        }
        recent_.push_back(line);
        while (recent_.size() > RecentCapacity()) recent_.pop_front();
        for (const auto& [id, sink] : sinks_) sinks.push_back(sink);
    }
    for (const auto& sink : sinks) (*sink)(line);
}

int Logger::AddSink(Sink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int id = next_sink_++;
    sinks_.push_back({id, std::make_shared<Sink>(std::move(sink))});
    return id;
}

void Logger::RemoveSink(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(sinks_, [&](const auto& s) { return s.first == id; });
}

std::vector<LogLine> Logger::Recent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<LogLine>(recent_.begin(), recent_.end());
}

void Logger::ClearRecent() {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_.clear();
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
