#include "aether/script/water_api.h"

#include <limits>

#if AETHER_KIT_WATER
#include "aether/water/water_system.h"
#endif

namespace aether::script {

#if AETHER_KIT_WATER

void InstallWaterApi(LuauHost& host) {
    using aether::water::SurfaceSample;
    using aether::water::WaterSystem;
    host.RegisterNative("Water", "HeightAt", [](NativeCall& c) {
        if (!c.IsNumber(0) || !c.IsNumber(1)) {
            c.Fail("Water.HeightAt(x, z) expected");
            return;
        }
        const WaterSystem* system = WaterSystem::Active();
        c.Return(static_cast<f64>(system != nullptr ? system->HeightAt(static_cast<f32>(c.Number(0)), static_cast<f32>(c.Number(1))) : std::numeric_limits<f32>::lowest()));
    });
    host.RegisterNative("Water", "IsWater", [](NativeCall& c) {
        if (!c.IsNumber(0) || !c.IsNumber(1)) {
            c.Fail("Water.IsWater(x, z) expected");
            return;
        }
        const WaterSystem* system = WaterSystem::Active();
        SurfaceSample sample;
        c.Return(system != nullptr && system->SurfaceAt(static_cast<f32>(c.Number(0)), static_cast<f32>(c.Number(1)), sample));
    });
    host.RegisterNative("Water", "Current", [](NativeCall& c) {
        if (!c.IsNumber(0) || !c.IsNumber(1)) {
            c.Fail("Water.Current(x, z) expected");
            return;
        }
        const WaterSystem* system = WaterSystem::Active();
        SurfaceSample sample;
        Vec3 current(0, 0, 0);
        if (system != nullptr && system->SurfaceAt(static_cast<f32>(c.Number(0)), static_cast<f32>(c.Number(1)), sample)) current = sample.current;
        c.ReturnVector(current);
    });
    host.RegisterNative("Water", "Submersion", [](NativeCall& c) {
        if (!c.IsEntity(0)) {
            c.Fail("Water.Submersion(entity) expected");
            return;
        }
        const WaterSystem* system = WaterSystem::Active();
        c.Return(static_cast<f64>(system != nullptr ? system->StateOf(c.EntityArg(0)).submersion : 0.0f));
    });
    host.RegisterNative("Water", "Drive", [](NativeCall& c) {
        if (!c.IsEntity(0) || !c.IsNumber(1) || !c.IsNumber(2)) {
            c.Fail("Water.Drive(boat, throttle, steer) expected");
            return;
        }
        WaterSystem* system = WaterSystem::Active();
        c.Return(system != nullptr && system->Drive(c.EntityArg(0), static_cast<f32>(c.Number(1)), static_cast<f32>(c.Number(2))));
    });
    host.RegisterNative("Water", "Time", [](NativeCall& c) {
        const WaterSystem* system = WaterSystem::Active();
        c.Return(static_cast<f64>(system != nullptr ? system->Time() : 0.0f));
    });
}

#else

void InstallWaterApi(LuauHost&) {}

#endif

} // namespace aether::script
