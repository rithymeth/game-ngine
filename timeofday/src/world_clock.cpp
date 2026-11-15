#include "aether/timeofday/world_clock.h"

#include <cmath>
#include <cstdio>

namespace aether::tod {

namespace {
WorldClock* g_active = nullptr;
constexpr f64 kPi = 3.14159265358979323846;
} // namespace

const char* PhaseName(Phase phase) {
    switch (phase) {
        case Phase::Night: return "night";
        case Phase::Dawn: return "dawn";
        case Phase::Day: return "day";
        case Phase::Dusk: return "dusk";
    }
    return "night";
}

WorldClock::WorldClock() { MakeActive(); }
WorldClock::~WorldClock() {
    if (g_active == this) g_active = nullptr;
}
WorldClock* WorldClock::Active() { return g_active; }
void WorldClock::MakeActive() { g_active = this; }

void WorldClock::Advance(f64 dt) {
    if (!(dt > 0.0) || !std::isfinite(dt) || scale_ <= 0.0) return;
    hour_ += dt * scale_ / day_length_ * 24.0;
    if (hour_ >= 24.0) {
        const f64 days = std::floor(hour_ / 24.0);
        hour_ -= days * 24.0;
        if (hour_ < 0.0 || hour_ >= 24.0) hour_ = 0.0; // rounding at the edge
        day_ += static_cast<i64>(days);
    }
}

Phase WorldClock::CurrentPhase() const {
    if (hour_ >= 5.0 && hour_ < 8.0) return Phase::Dawn;
    if (hour_ >= 8.0 && hour_ < 18.0) return Phase::Day;
    if (hour_ >= 18.0 && hour_ < 21.0) return Phase::Dusk;
    return Phase::Night;
}

f64 WorldClock::Sunlight() const { return 0.5 - 0.5 * std::cos(2.0 * kPi * hour_ / 24.0); }

std::string WorldClock::TimeText() const {
    const i32 minutes = static_cast<i32>(std::floor(hour_ * 60.0));
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d", (minutes / 60) % 24, minutes % 60);
    return text;
}

bool WorldClock::SetHour(f64 hour) {
    if (!std::isfinite(hour)) return false;
    hour = std::fmod(hour, 24.0);
    if (hour < 0.0) hour += 24.0;
    if (hour >= 24.0) hour = 0.0;
    hour_ = hour;
    return true;
}

bool WorldClock::SetDay(i64 day) {
    if (day < 0) return false;
    day_ = day;
    return true;
}

bool WorldClock::SetDayLength(f64 seconds) {
    if (!std::isfinite(seconds) || seconds < kMinDayLength || seconds > kMaxDayLength) return false;
    day_length_ = seconds;
    return true;
}

bool WorldClock::SetScale(f64 scale) {
    if (!std::isfinite(scale) || scale < 0.0 || scale > kMaxScale) return false;
    scale_ = scale;
    return true;
}

} // namespace aether::tod
