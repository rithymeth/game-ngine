#include "aether/script/fire_api.h"

#if AETHER_KIT_FIRE
#include "aether/fire/fire_system.h"
#endif

namespace aether::script {

#if AETHER_KIT_FIRE

void InstallFireApi(LuauHost& host) {
    using aether::fire::FireSystem;
    const auto entity_call = [&](const char* name, const char* usage, bool (FireSystem::*fn)(Entity)) {
        host.RegisterNative("Fire", name, [fn, usage](NativeCall& c) {
            if (!c.IsEntity(0)) {
                c.Fail(usage);
                return;
            }
            FireSystem* system = FireSystem::Active();
            c.Return(system != nullptr && (system->*fn)(c.EntityArg(0)));
        });
    };
    entity_call("Ignite", "Fire.Ignite(entity) expected", &FireSystem::Ignite);
    entity_call("Extinguish", "Fire.Extinguish(entity) expected", &FireSystem::Extinguish);
    const auto query = [&](const char* name, const char* usage, bool (FireSystem::*fn)(Entity) const) {
        host.RegisterNative("Fire", name, [fn, usage](NativeCall& c) {
            if (!c.IsEntity(0)) {
                c.Fail(usage);
                return;
            }
            const FireSystem* system = FireSystem::Active();
            c.Return(system != nullptr && (system->*fn)(c.EntityArg(0)));
        });
    };
    query("IsBurning", "Fire.IsBurning(entity) expected", &FireSystem::IsBurning);
    query("IsBurnt", "Fire.IsBurnt(entity) expected", &FireSystem::IsBurnt);
    const auto number = [&](const char* name, const char* usage, f32 (FireSystem::*fn)(Entity) const) {
        host.RegisterNative("Fire", name, [fn, usage](NativeCall& c) {
            if (!c.IsEntity(0)) {
                c.Fail(usage);
                return;
            }
            const FireSystem* system = FireSystem::Active();
            c.Return(static_cast<f64>(system != nullptr ? (system->*fn)(c.EntityArg(0)) : 0.0f));
        });
    };
    number("Heat", "Fire.Heat(entity) expected", &FireSystem::Heat);
    number("FuelLeft", "Fire.FuelLeft(entity) expected", &FireSystem::FuelLeft);
    host.RegisterNative("Fire", "IgniteNear", [](NativeCall& c) {
        if (!c.IsNumber(0) || !c.IsNumber(1) || !c.IsNumber(2) || !c.IsNumber(3)) {
            c.Fail("Fire.IgniteNear(x, y, z, radius) expected");
            return;
        }
        FireSystem* system = FireSystem::Active();
        const Vec3 centre(static_cast<f32>(c.Number(0)), static_cast<f32>(c.Number(1)), static_cast<f32>(c.Number(2)));
        c.Return(static_cast<f64>(system != nullptr ? system->IgniteNear(centre, static_cast<f32>(c.Number(3))) : 0));
    });
    host.RegisterNative("Fire", "Burning", [](NativeCall& c) {
        const FireSystem* system = FireSystem::Active();
        c.Return(static_cast<f64>(system != nullptr ? system->BurningCount() : 0));
    });
    host.RegisterNative("Fire", "SetWetness", [](NativeCall& c) {
        if (!c.IsEntity(0) || !c.IsNumber(1)) {
            c.Fail("Fire.SetWetness(entity, wetness) expected");
            return;
        }
        FireSystem* system = FireSystem::Active();
        c.Return(system != nullptr && system->SetWetness(c.EntityArg(0), static_cast<f32>(c.Number(1))));
    });
    host.RegisterNative("Fire", "WetAll", [](NativeCall& c) {
        if (!c.IsNumber(0)) {
            c.Fail("Fire.WetAll(wetness) expected");
            return;
        }
        FireSystem* system = FireSystem::Active();
        c.Return(static_cast<f64>(system != nullptr ? system->WetAll(static_cast<f32>(c.Number(0))) : 0));
    });
}

#else

void InstallFireApi(LuauHost&) {}

#endif

} // namespace aether::script
