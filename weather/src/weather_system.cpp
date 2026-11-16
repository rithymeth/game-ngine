#include "aether/weather/weather_system.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace aether::weather {

namespace {

WeatherSystem* g_active = nullptr;

constexpr f64 kStep = 0.025;       // weather moves in steps of this many seconds whatever the frame rate, so the same seed gives the same weather at 30 or 144 fps
constexpr f64 kMaxAdvance = 3600.0; // the most one Advance call covers
constexpr f64 kTwoPi = 6.283185307179586;

struct KindInfo {
    const char* name;
    WeatherParams params;
};

//                                           clouds rain  snow  fog   wind light  lightning
constexpr KindInfo kKinds[kWeatherKindCount] = {
    {"clear",         {0.05f, 0.0f, 0.0f, 0.00f, 2.0f,  1.00f, 0.0f}},
    {"partly_cloudy", {0.45f, 0.0f, 0.0f, 0.05f, 4.0f,  0.85f, 0.0f}},
    {"overcast",      {0.85f, 0.0f, 0.0f, 0.15f, 5.0f,  0.55f, 0.0f}},
    {"rain",          {0.90f, 0.6f, 0.0f, 0.30f, 8.0f,  0.40f, 0.0f}},
    {"storm",         {1.00f, 1.0f, 0.0f, 0.45f, 18.0f, 0.25f, 6.0f}},
    {"fog",           {0.70f, 0.0f, 0.0f, 1.00f, 1.0f,  0.50f, 0.0f}},
    {"snow",          {0.90f, 0.0f, 0.7f, 0.35f, 4.0f,  0.50f, 0.0f}},
};

// What tends to follow what, as weights (rows: from, columns: to, in the order of WeatherKind). Nothing follows itself.
//                                 clear partly overcast rain storm fog snow
constexpr f64 kFollows[kWeatherKindCount][kWeatherKindCount] = {
    /* clear        */ {0, 6, 2, 0, 0, 1, 0},
    /* partly       */ {4, 0, 4, 1, 0, 0, 0},
    /* overcast     */ {0, 3, 0, 4, 0, 1, 1},
    /* rain         */ {0, 2, 4, 0, 2, 0, 0},
    /* storm        */ {0, 0, 2, 6, 0, 0, 0},
    /* fog          */ {3, 2, 2, 0, 0, 0, 0},
    /* snow         */ {1, 2, 3, 0, 0, 0, 0},
};

f32 Lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

WeatherParams Mix(const WeatherParams& a, const WeatherParams& b, f32 t) {
    return {Lerp(a.clouds, b.clouds, t), Lerp(a.rain, b.rain, t), Lerp(a.snow, b.snow, t), Lerp(a.fog, b.fog, t), Lerp(a.wind, b.wind, t),
            Lerp(a.light_scale, b.light_scale, t), Lerp(a.lightning, b.lightning, t)};
}

u64 SplitMix(u64& state) {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace

const char* WeatherName(WeatherKind kind) {
    const u32 i = static_cast<u32>(kind);
    return i < kWeatherKindCount ? kKinds[i].name : "";
}

bool ParseWeather(std::string_view name, WeatherKind& out) {
    std::string lower;
    for (const char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (u32 i = 0; i < kWeatherKindCount; ++i) {
        if (lower == kKinds[i].name) {
            out = static_cast<WeatherKind>(i);
            return true;
        }
    }
    return false;
}

WeatherParams ProfileOf(WeatherKind kind) {
    const u32 i = static_cast<u32>(kind);
    return i < kWeatherKindCount ? kKinds[i].params : WeatherParams{};
}

WeatherSystem::WeatherSystem() {
    now_ = from_ = ProfileOf(WeatherKind::Clear);
    SetSeed(1);
    MakeActive();
}

WeatherSystem::~WeatherSystem() {
    if (g_active == this) g_active = nullptr;
}

WeatherSystem* WeatherSystem::Active() { return g_active; }
void WeatherSystem::MakeActive() { g_active = this; }

u64 WeatherSystem::NextRandom() { return SplitMix(rng_); }
f64 WeatherSystem::NextUnit() { return static_cast<f64>(NextRandom() >> 11) * (1.0 / 9007199254740992.0); }

void WeatherSystem::SetSeed(u64 seed) {
    seed_ = seed;
    rng_ = seed * 0x2545F4914F6CDD1Dull + 0x1234567ull;
    for (f64& phase : gust_phase_) phase = NextUnit() * kTwoPi;
    hazard_ = 0.0;
    strike_budget_ = -std::log(1.0 - NextUnit());
    if (auto_) ScheduleChange();
}

void WeatherSystem::SetAuto(bool on) {
    auto_ = on;
    if (on) ScheduleChange();
}

bool WeatherSystem::SetMeanDuration(f64 seconds) {
    if (!std::isfinite(seconds) || seconds < kMinMeanDuration || seconds > kMaxMeanDuration) return false;
    mean_duration_ = seconds;
    if (auto_) ScheduleChange();
    return true;
}

bool WeatherSystem::SetWindDirection(f64 yaw_degrees) {
    if (!std::isfinite(yaw_degrees)) return false;
    wind_yaw_ = std::fmod(yaw_degrees, 360.0);
    if (wind_yaw_ < 0.0) wind_yaw_ += 360.0;
    return true;
}

void WeatherSystem::ScheduleChange() { until_change_ = mean_duration_ * (0.5 + NextUnit()); }

void WeatherSystem::BeginTransition(WeatherKind kind, f64 seconds) {
    from_ = now_;
    current_ = target_;
    target_ = kind;
    transition_ = seconds;
    elapsed_ = 0.0;
    if (seconds <= 0.0) {
        current_ = kind;
        now_ = ProfileOf(kind);
    }
}

bool WeatherSystem::Set(WeatherKind kind, f64 seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > kMaxTransition || static_cast<u32>(kind) >= kWeatherKindCount) return false;
    BeginTransition(kind, seconds);
    if (auto_) ScheduleChange();
    return true;
}

void WeatherSystem::PickNextKind() {
    const f64* row = kFollows[static_cast<u32>(target_)];
    f64 total = 0.0;
    for (u32 i = 0; i < kWeatherKindCount; ++i) total += row[i];
    WeatherKind kind = WeatherKind::Clear;
    if (total > 0.0) {
        f64 pick = NextUnit() * total;
        for (u32 i = 0; i < kWeatherKindCount; ++i) {
            if (row[i] <= 0.0) continue;
            kind = static_cast<WeatherKind>(i);
            if (pick < row[i]) break;
            pick -= row[i];
        }
    }
    // Rain and storms fall as snow in the cold, and snow needs the cold.
    if (cold_ && (kind == WeatherKind::Rain || kind == WeatherKind::Storm)) kind = WeatherKind::Snow;
    if (!cold_ && kind == WeatherKind::Snow) kind = WeatherKind::Rain;
    BeginTransition(kind, std::min(30.0, 0.25 * mean_duration_));
}

f64 WeatherSystem::Blend() const {
    if (transition_ <= 0.0) return 1.0;
    return std::clamp(elapsed_ / transition_, 0.0, 1.0);
}

void WeatherSystem::Step(f64 dt) {
    time_ += dt;
    // The blend.
    if (elapsed_ < transition_) {
        elapsed_ = std::min(elapsed_ + dt, transition_);
        const f32 t = static_cast<f32>(Blend());
        const f32 smooth = t * t * (3.0f - 2.0f * t);
        now_ = Mix(from_, ProfileOf(target_), smooth);
        if (elapsed_ >= transition_) {
            now_ = ProfileOf(target_);
            current_ = target_;
        }
    }
    // The weather changing by itself.
    if (auto_) {
        until_change_ -= dt;
        if (until_change_ <= 0.0) {
            PickNextKind();
            ScheduleChange();
        }
    }
    // Lightning: strikes arrive at random at the current rate, which changes as the weather blends; the "hazard" is the rate added up over time, and a strike comes
    // each time it passes a random amount.
    flash_ = flash_ * static_cast<f32>(std::exp(-6.0 * dt));
    if (flash_ < 0.001f) flash_ = 0.0f;
    since_strike_ += dt;
    const f64 per_second = static_cast<f64>(now_.lightning) / 60.0;
    if (per_second > 1e-6) {
        hazard_ += per_second * dt;
        if (hazard_ >= strike_budget_) {
            flash_ = 1.0f;
            ++strikes_;
            since_strike_ = 0.0;
            thunder_delay_ = (300.0 + 2700.0 * NextUnit()) / kSpeedOfSound; // 0.3 to 3 km away
            hazard_ = 0.0;
            strike_budget_ = -std::log(1.0 - NextUnit());
        }
    }
    // The ground: wet by rain (and a little by snow), drying out slowly when it is not.
    const f64 wetting = static_cast<f64>(now_.rain) + 0.3 * static_cast<f64>(now_.snow);
    if (wetting > 0.05) wetness_ = static_cast<f32>(std::min(1.0, static_cast<f64>(wetness_) + wetting * dt / kSoakSeconds));
    else wetness_ = static_cast<f32>(std::max(0.0, static_cast<f64>(wetness_) - dt / kDrySeconds));
}

void WeatherSystem::Advance(f64 dt) {
    if (!std::isfinite(dt) || dt <= 0.0) return;
    accumulated_ += std::min(dt, kMaxAdvance);
    while (accumulated_ >= kStep) {
        Step(kStep);
        accumulated_ -= kStep;
    }
}

f32 WeatherSystem::WindSpeed() const {
    const f64 gust = 1.0 + 0.45 * std::sin(0.9 * time_ + gust_phase_[0]) + 0.25 * std::sin(2.3 * time_ + gust_phase_[1]) + 0.12 * std::sin(5.7 * time_ + gust_phase_[2]);
    return static_cast<f32>(std::max(0.0, static_cast<f64>(now_.wind) * gust));
}

Vec3 WeatherSystem::WindVector() const {
    const f64 yaw = wind_yaw_ * 3.14159265358979323846 / 180.0;
    const f32 speed = WindSpeed();
    return Vec3(static_cast<f32>(std::sin(yaw)) * speed, 0.0f, static_cast<f32>(std::cos(yaw)) * speed);
}

f64 WeatherSystem::SecondsSinceStrike() const { return since_strike_; }

} // namespace aether::weather
