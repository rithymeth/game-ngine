#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"

#include <string>
#include <string_view>

namespace aether::weather {

enum class WeatherKind : u8 { Clear, PartlyCloudy, Overcast, Rain, Storm, Fog, Snow };
constexpr u32 kWeatherKindCount = 7;
// "clear", "partly_cloudy", "overcast", "rain", "storm", "fog", "snow"
const char* WeatherName(WeatherKind kind);
// Accepts those names in any case. False (leaving `out` alone) for anything else.
bool ParseWeather(std::string_view name, WeatherKind& out);

// What the sky is doing, as numbers. Each is 0 up to its top; the blend between two kinds is a smooth step between these.
struct WeatherParams {
    f32 clouds = 0.0f;          // cloud cover, 0 clear to 1 solid
    f32 rain = 0.0f;            // rain intensity, 0 to 1
    f32 snow = 0.0f;            // snow intensity, 0 to 1
    f32 fog = 0.0f;             // fog thickness, 0 to 1 (scale a fog density with it)
    f32 wind = 0.0f;            // the steady wind, m/s (gusts come on top)
    f32 light_scale = 1.0f;     // how much of the sun gets through, 0 to 1 (multiply a light or a sky with it)
    f32 lightning = 0.0f;       // strikes per minute
};
// The numbers a kind of weather stands for.
WeatherParams ProfileOf(WeatherKind kind);

// The game's weather: one kind at a time, blending over a few seconds to the next, which the game sets or which comes on its own. Like the clock, it belongs to
// the game, not the scene: it keeps going through a scene change. Deterministic: with the same seed and the same dt it does the same, every time.
//
//   - Set(kind, seconds) blends to a kind from wherever the numbers are now (an interrupted blend carries on smoothly, never jumps).
//   - SetAuto(true) lets it change by itself: after a random time of 0.5 to 1.5 times the mean duration it picks the next kind from a table of what usually follows
//     what (clear tends to become cloudy, rain to a storm or clearing), from the seed.
//   - The wind has a direction, and gusts that make its speed swell and drop; lightning strikes arrive at random while `lightning` is above 0, each with a flash that
//     fades fast and a thunder delay for how far away it was; the ground gets wet in rain (and a little in snow) and dries out over minutes.
class WeatherSystem {
public:
    static constexpr f64 kMaxTransition = 3600.0;      // seconds
    static constexpr f64 kMinMeanDuration = 1.0;       // seconds
    static constexpr f64 kMaxMeanDuration = 1.0e6;
    static constexpr f64 kSoakSeconds = 30.0;          // steady full rain wets the ground this fast
    static constexpr f64 kDrySeconds = 300.0;          // a wet ground dries out this slowly
    static constexpr f64 kSpeedOfSound = 343.0;        // m/s

    WeatherSystem();
    ~WeatherSystem();
    WeatherSystem(const WeatherSystem&) = delete;
    WeatherSystem& operator=(const WeatherSystem&) = delete;

    static WeatherSystem* Active();
    void MakeActive();

    // Moves everything on by `dt` real seconds (ignored when not a positive number; at most an hour per call). Time passes in fixed steps of 25 ms whatever dt is, so the
    // same seed gives the same weather at any frame rate; what is left over waits for the next call.
    void Advance(f64 dt);

    // Blends to `kind` over `seconds` (0 is at once, up to kMaxTransition). False (nothing changes) for a time that is not a number or is out of range.
    bool Set(WeatherKind kind, f64 seconds = 30.0);
    // Automatic changes on or off. Turning it on starts the countdown to the first change; a manual Set also restarts it.
    void SetAuto(bool on);
    bool IsAuto() const { return auto_; }
    // The seed the automatic changes, the gusts and the lightning come from (call it before anything is wanted from them: it restarts them). Mean time between changes.
    void SetSeed(u64 seed);
    u64 Seed() const { return seed_; }
    bool SetMeanDuration(f64 seconds);
    f64 MeanDuration() const { return mean_duration_; }
    // In a cold place rain and storms fall as snow.
    void SetCold(bool cold) { cold_ = cold; }
    bool IsCold() const { return cold_; }
    // Which way the wind blows towards, in degrees about +Y (0 blows towards +Z, 90 towards +X). False for a number that is not finite.
    bool SetWindDirection(f64 yaw_degrees);
    f64 WindDirection() const { return wind_yaw_; }

    WeatherKind Current() const { return current_; } // the kind it is at, or has been blending away from since the last Set
    WeatherKind Target() const { return target_; }   // the kind it is going to (the same when settled)
    f64 Blend() const;                               // 0 just set, 1 arrived
    WeatherParams Params() const { return now_; }    // the blended numbers
    f32 Clouds() const { return now_.clouds; }
    f32 Rain() const { return now_.rain; }
    f32 Snow() const { return now_.snow; }
    f32 Fog() const { return now_.fog; }
    f32 LightScale() const { return now_.light_scale; }
    f32 WindSpeed() const;                           // steady wind plus the gust now, m/s, never below 0
    Vec3 WindVector() const;                         // WindSpeed() along WindDirection(): the push on a flag, a leaf, a cloth
    f32 Lightning() const { return flash_; }         // 1 at a strike, fading to 0 within a second or so: the brightness of the flash
    u32 StrikeCount() const { return strikes_; }
    f64 SecondsSinceStrike() const;                  // a very large number before the first
    f64 ThunderDelay() const { return thunder_delay_; } // seconds from the last flash to its thunder (0 before any strike)
    f32 Wetness() const { return wetness_; }         // 0 dry to 1 soaked: for darkening and glossing surfaces
    f64 Time() const { return time_; }               // seconds of weather so far

private:
    void Step(f64 dt);
    u64 NextRandom();
    f64 NextUnit(); // 0 up to 1
    void ScheduleChange();
    void PickNextKind();
    void BeginTransition(WeatherKind kind, f64 seconds);

    WeatherKind current_ = WeatherKind::Clear;
    WeatherKind target_ = WeatherKind::Clear;
    WeatherParams from_{};   // the numbers when the blend began
    WeatherParams now_{};
    f64 transition_ = 0.0;   // seconds the blend takes
    f64 elapsed_ = 0.0;      // seconds into it
    bool auto_ = false;
    bool cold_ = false;
    u64 seed_ = 1;
    u64 rng_ = 0;
    f64 mean_duration_ = 300.0;
    f64 until_change_ = 0.0; // seconds to the next automatic change
    f64 wind_yaw_ = 0.0;
    f64 gust_phase_[3] = {0, 0, 0};
    f64 time_ = 0.0;
    f64 accumulated_ = 0.0;  // time handed to Advance that has not made a whole step yet
    f64 hazard_ = 0.0;       // lightning: the strike rate added up since the last strike
    f64 strike_budget_ = 1.0; // and how much of it the next strike waits for
    f32 flash_ = 0.0f;
    u32 strikes_ = 0;
    f64 since_strike_ = 1.0e12;
    f64 thunder_delay_ = 0.0;
    f32 wetness_ = 0.0f;
};

} // namespace aether::weather
