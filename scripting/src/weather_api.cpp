#include "aether/script/weather_api.h"

#if AETHER_KIT_WEATHER
#include "aether/weather/weather_system.h"

#include <cmath>
#endif

namespace aether::script {

#if AETHER_KIT_WEATHER

namespace {

using namespace aether::weather;

bool NeedNumber(NativeCall& c, usize i, const char* what) {
    if (c.IsNumber(i)) return true;
    c.Fail(std::string("Weather: ") + what + " must be a number");
    return false;
}

} // namespace

void InstallWeatherApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("Weather", name, std::move(fn)); };
    const auto number = [&](const char* name, f64 (*get)(const WeatherSystem&)) {
        def(name, [get](NativeCall& c) {
            const WeatherSystem* w = WeatherSystem::Active();
            c.Return(w ? get(*w) : 0.0);
        });
    };
    number("Blend", [](const WeatherSystem& w) { return w.Blend(); });
    number("Clouds", [](const WeatherSystem& w) { return static_cast<f64>(w.Clouds()); });
    number("Rain", [](const WeatherSystem& w) { return static_cast<f64>(w.Rain()); });
    number("Snow", [](const WeatherSystem& w) { return static_cast<f64>(w.Snow()); });
    number("Fog", [](const WeatherSystem& w) { return static_cast<f64>(w.Fog()); });
    number("LightScale", [](const WeatherSystem& w) { return static_cast<f64>(w.LightScale()); });
    number("WindSpeed", [](const WeatherSystem& w) { return static_cast<f64>(w.WindSpeed()); });
    number("WindDirection", [](const WeatherSystem& w) { return w.WindDirection(); });
    number("Lightning", [](const WeatherSystem& w) { return static_cast<f64>(w.Lightning()); });
    number("Strikes", [](const WeatherSystem& w) { return static_cast<f64>(w.StrikeCount()); });
    number("SecondsSinceStrike", [](const WeatherSystem& w) { return w.SecondsSinceStrike(); });
    number("ThunderDelay", [](const WeatherSystem& w) { return w.ThunderDelay(); });
    number("Wetness", [](const WeatherSystem& w) { return static_cast<f64>(w.Wetness()); });
    def("Current", [](NativeCall& c) {
        const WeatherSystem* w = WeatherSystem::Active();
        c.Return(std::string(w ? WeatherName(w->Current()) : ""));
    });
    def("Target", [](NativeCall& c) {
        const WeatherSystem* w = WeatherSystem::Active();
        c.Return(std::string(w ? WeatherName(w->Target()) : ""));
    });
    def("WindVector", [](NativeCall& c) {
        const WeatherSystem* w = WeatherSystem::Active();
        c.ReturnVector(w ? w->WindVector() : Vec3(0, 0, 0));
    });
    def("Set", [](NativeCall& c) {
        if (!c.IsString(0)) {
            c.Fail("Weather.Set(kind [, seconds]) expected a kind name");
            return;
        }
        if (c.Count() > 1 && !c.IsNumber(1)) {
            c.Fail("Weather.Set: the time must be a number");
            return;
        }
        WeatherSystem* w = WeatherSystem::Active();
        WeatherKind kind;
        c.Return(w != nullptr && ParseWeather(c.String(0), kind) && w->Set(kind, c.Count() > 1 ? c.Number(1) : 30.0));
    });
    def("SetAuto", [](NativeCall& c) {
        if (!c.IsBool(0)) {
            c.Fail("Weather.SetAuto(on [, seed]) expected a boolean");
            return;
        }
        WeatherSystem* w = WeatherSystem::Active();
        if (w == nullptr) return;
        if (c.Count() > 1) {
            if (!NeedNumber(c, 1, "the seed")) return;
            const f64 seed = c.Number(1);
            if (std::isfinite(seed) && seed >= 0.0 && seed < 1.8e19) w->SetSeed(static_cast<u64>(seed));
        }
        w->SetAuto(c.Bool(0));
    });
    def("SetCold", [](NativeCall& c) {
        if (!c.IsBool(0)) {
            c.Fail("Weather.SetCold(cold) expected a boolean");
            return;
        }
        if (WeatherSystem* w = WeatherSystem::Active()) w->SetCold(c.Bool(0));
    });
    def("SetWindDirection", [](NativeCall& c) {
        if (!NeedNumber(c, 0, "the direction")) return;
        WeatherSystem* w = WeatherSystem::Active();
        c.Return(w != nullptr && w->SetWindDirection(c.Number(0)));
    });
    def("SetMeanDuration", [](NativeCall& c) {
        if (!NeedNumber(c, 0, "the duration")) return;
        WeatherSystem* w = WeatherSystem::Active();
        c.Return(w != nullptr && w->SetMeanDuration(c.Number(0)));
    });
}

#else

void InstallWeatherApi(LuauHost&) {}

#endif

} // namespace aether::script
