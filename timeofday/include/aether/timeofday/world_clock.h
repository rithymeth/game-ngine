#pragma once

#include "aether/core/base.h"

#include <string>

namespace aether::tod {

enum class Phase : u8 { Night, Dawn, Day, Dusk };
// "night", "dawn", "day", "dusk"
const char* PhaseName(Phase phase);

// The game's clock: an hour of the day (0 up to 24) and a day count, advanced by the frame's time. One real-time `day_length` (seconds, default
// 20 minutes) is one game day; `scale` speeds it up or slows it down (0 pauses). The clock belongs to the game, not the scene: it keeps running
// through a scene change. Deterministic: it only moves by the dt it is given.
//
//   dawn  5:00 up to 8:00     day  8:00 up to 18:00     dusk  18:00 up to 21:00     night  21:00 up to 5:00
class WorldClock {
public:
    static constexpr f64 kMinDayLength = 1.0;     // seconds
    static constexpr f64 kMaxDayLength = 1.0e6;
    static constexpr f64 kMaxScale = 1000.0;

    WorldClock();
    ~WorldClock();
    WorldClock(const WorldClock&) = delete;
    WorldClock& operator=(const WorldClock&) = delete;

    static WorldClock* Active();
    void MakeActive();

    // Moves the clock by `dt` real seconds (ignored when not a positive number). Passing midnight counts a day.
    void Advance(f64 dt);

    f64 Hour() const { return hour_; }
    i64 Day() const { return day_; }
    f64 TotalHours() const { return static_cast<f64>(day_) * 24.0 + hour_; } // for measuring how long ago something was
    Phase CurrentPhase() const;
    bool IsNight() const { return CurrentPhase() == Phase::Night; }
    // How bright the sun is: 0 at midnight, 1 at noon, smooth between (0.5 at 6:00 and 18:00).
    f64 Sunlight() const;
    std::string TimeText() const; // "07:05"

    // Each setter refuses what makes no sense (and changes nothing): the hour wraps into 0..24 but must be a number; the day is 0 or more.
    bool SetHour(f64 hour);
    bool SetDay(i64 day);
    bool SetDayLength(f64 seconds);
    bool SetScale(f64 scale);
    f64 DayLength() const { return day_length_; }
    f64 Scale() const { return scale_; }

private:
    f64 hour_ = 8.0;
    i64 day_ = 0;
    f64 day_length_ = 1200.0;
    f64 scale_ = 1.0;
};

} // namespace aether::tod
