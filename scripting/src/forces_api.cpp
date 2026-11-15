#include "aether/script/forces_api.h"

#if AETHER_KIT_FORCEFIELDS
#include "aether/forcefields/force_field_system.h"
#endif

namespace aether::script {

#if AETHER_KIT_FORCEFIELDS

void InstallForcesApi(LuauHost& host) {
    host.RegisterNative("Forces", "Explode", [](NativeCall& c) {
        if (!c.IsVector(0) || !c.IsNumber(1) || !c.IsNumber(2)) {
            c.Fail("Forces.Explode(position, radius, strength [, upward]) expected");
            return;
        }
        aether::ff::ForceFieldSystem* system = aether::ff::ForceFieldSystem::Active();
        const f32 upward = c.IsNumber(3) ? static_cast<f32>(c.Number(3)) : 0.0f;
        c.Return(static_cast<f64>(system ? system->Explode(c.Vector(0), static_cast<f32>(c.Number(1)), static_cast<f32>(c.Number(2)), upward) : 0));
    });
}

#else

void InstallForcesApi(LuauHost&) {}

#endif

} // namespace aether::script
