#include "aether/script/timeofday_api.h"

#if AETHER_KIT_TIMEOFDAY
#include "aether/timeofday/world_clock.h"

#include <cmath>
#endif

namespace aether::script {

#if AETHER_KIT_TIMEOFDAY

namespace {

using namespace aether::tod;

bool NeedNumber(NativeCall& c, const char* what) {
    if (c.IsNumber(0)) return true;
    c.Fail(std::string("TimeOfDay: ") + what + " must be a number");
    return false;
}

} // namespace

void InstallTimeOfDayApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("TimeOfDay", name, std::move(fn)); };
    const auto number = [&](const char* name, f64 (*get)(const WorldClock&)) {
        def(name, [get](NativeCall& c) {
            const WorldClock* clock = WorldClock::Active();
            c.Return(clock ? get(*clock) : 0.0);
        });
    };
    number("GetHour", [](const WorldClock& w) { return w.Hour(); });
    number("GetDay", [](const WorldClock& w) { return static_cast<f64>(w.Day()); });
    number("GetTotalHours", [](const WorldClock& w) { return w.TotalHours(); });
    number("Sunlight", [](const WorldClock& w) { return w.Sunlight(); });
    number("GetDayLength", [](const WorldClock& w) { return w.DayLength(); });
    number("GetScale", [](const WorldClock& w) { return w.Scale(); });
    def("GetPhase", [](NativeCall& c) {
        const WorldClock* clock = WorldClock::Active();
        c.Return(std::string(clock ? PhaseName(clock->CurrentPhase()) : ""));
    });
    def("IsNight", [](NativeCall& c) {
        const WorldClock* clock = WorldClock::Active();
        c.Return(clock != nullptr && clock->IsNight());
    });
    def("GetTimeText", [](NativeCall& c) {
        const WorldClock* clock = WorldClock::Active();
        c.Return(clock ? clock->TimeText() : std::string());
    });
    def("SetHour", [](NativeCall& c) {
        if (!NeedNumber(c, "the hour")) return;
        WorldClock* clock = WorldClock::Active();
        c.Return(clock != nullptr && clock->SetHour(c.Number(0)));
    });
    def("SetDay", [](NativeCall& c) {
        if (!NeedNumber(c, "the day")) return;
        WorldClock* clock = WorldClock::Active();
        const f64 day = c.Number(0);
        c.Return(clock != nullptr && std::isfinite(day) && day == std::floor(day) && day >= 0.0 && day < 9.0e15 && clock->SetDay(static_cast<i64>(day)));
    });
    def("SetDayLength", [](NativeCall& c) {
        if (!NeedNumber(c, "the day length")) return;
        WorldClock* clock = WorldClock::Active();
        c.Return(clock != nullptr && clock->SetDayLength(c.Number(0)));
    });
    def("SetScale", [](NativeCall& c) {
        if (!NeedNumber(c, "the scale")) return;
        WorldClock* clock = WorldClock::Active();
        c.Return(clock != nullptr && clock->SetScale(c.Number(0)));
    });
}

#else

void InstallTimeOfDayApi(LuauHost&) {}

#endif

} // namespace aether::script
