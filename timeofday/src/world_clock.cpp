#include "aether/timeofday/world_clock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace aether::tod {

namespace {
WorldClock* g_active = nullptr;
constexpr f64 kPi = 3.14159265358979323846;

AETHER_NOINLINE void AdvanceWrapped(WorldClock& clock, f64 dt, f64 seconds) {
    const f64 day_length = clock.DayLength();
    const f64 scale = clock.Scale();
    constexpr i64 kLastDay = std::numeric_limits<i64>::max();
    const f64 estimated_days = std::floor(seconds / day_length);
    if (estimated_days >= 0x1p64) {
        // This jump always saturates the day count. Split scaled seconds into
        // a product and its rounding error, reducing both before adding.
        // If the product overflowed, scale the input down by an exact power
        // of two (larger than kMaxScale) and restore it after reduction.
        constexpr f64 kOverflowDivisor = 0x1p10;
        static_assert(WorldClock::kMaxScale <= kOverflowDivisor);
        const bool overflowed = !std::isfinite(seconds);
        const f64 restore = overflowed ? kOverflowDivisor : 1.0;
        const f64 input = dt / restore;
        const f64 high = overflowed ? input * scale : seconds;
        const f64 low = std::fma(input, scale, -high);
        const auto remainder = [&](f64 part) {
            return std::fmod(std::fmod(part, day_length) * restore, day_length);
        };
        const f64 fraction = std::fmod(remainder(high) + remainder(low), day_length) / day_length;
        clock.SetHour(clock.Hour() + fraction * 24.0);
        clock.SetDay(kLastDay);
        return;
    }

    // Recover any low bits lost in the product and the quotient before
    // adding the starting hour. At large magnitudes even that entire hour
    // can disappear if it is added before removing the complete days.
    const f64 residual = std::fma(-estimated_days, day_length, seconds) + std::fma(dt, scale, -seconds);
    f64 adjustment = std::floor(residual / day_length);
    f64 remainder = std::fma(-adjustment, day_length, residual);
    // Division can round to an integer right beside a day boundary.
    if (remainder < 0.0) {
        --adjustment;
        remainder = std::min(remainder + day_length, std::nextafter(day_length, 0.0));
    } else if (remainder >= day_length) {
        ++adjustment;
        remainder -= day_length;
    }
    const f64 hour = clock.Hour() + remainder / day_length * 24.0;
    const bool crossed_midnight = hour >= 24.0;
    clock.SetHour(hour);

    const i64 correction = static_cast<i64>(adjustment) + (crossed_midnight ? 1 : 0);
    u64 days = static_cast<u64>(estimated_days);
    const u64 capacity = static_cast<u64>(kLastDay - clock.Day());
    if (correction < 0) {
        days -= static_cast<u64>(-correction);
    } else {
        const u64 extra = static_cast<u64>(correction);
        if (days > capacity || extra > capacity - days) {
            clock.SetDay(kLastDay);
            return;
        }
        days += extra;
    }
    clock.SetDay(clock.Day() + static_cast<i64>(std::min(days, capacity)));
}
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
    const f64 seconds = dt * scale_;
    const f64 next = hour_ + seconds / day_length_ * 24.0;
    if (next < 24.0) {
        hour_ = next;
        return;
    }
    AdvanceWrapped(*this, dt, seconds);
}

Phase WorldClock::CurrentPhase() const {
    if (hour_ >= 5.0 && hour_ < 8.0) return Phase::Dawn;
    if (hour_ >= 8.0 && hour_ < 18.0) return Phase::Day;
    if (hour_ >= 18.0 && hour_ < 21.0) return Phase::Dusk;
    return Phase::Night;
}

f64 WorldClock::Sunlight() const { return 0.5 - 0.5 * std::cos(2.0 * kPi * hour_ / 24.0); }

std::string WorldClock::TimeText() const {
    constexpr i32 kMinutesPerDay = 24 * 60;
    // An exact authored minute can multiply back to one representable value below
    // its boundary. Nudge the display value, but never roll it into the next day.
    const f64 display_minutes = std::nextafter(hour_ * 60.0, static_cast<f64>(kMinutesPerDay));
    const i32 minutes = std::min(kMinutesPerDay - 1, static_cast<i32>(std::floor(display_minutes)));
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d", minutes / 60, minutes % 60);
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
