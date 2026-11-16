#include "aether/script/destruction_api.h"

#if AETHER_KIT_DESTRUCTION
#include "aether/destruction/destruction_system.h"
#endif

namespace aether::script {

#if AETHER_KIT_DESTRUCTION

void InstallDestructionApi(LuauHost& host) {
    using aether::destruction::DestructionSystem;
    host.RegisterNative("Destruction", "Damage", [](NativeCall& c) {
        if (!c.IsEntity(0) || !c.IsNumber(1)) {
            c.Fail("Destruction.Damage(entity, amount [, point]) expected");
            return;
        }
        DestructionSystem* system = DestructionSystem::Active();
        const Vec3 point = c.IsVector(2) ? c.Vector(2) : Vec3(0, 0, 0);
        c.Return(system != nullptr && system->Damage(c.EntityArg(0), static_cast<f32>(c.Number(1)), c.IsVector(2) ? &point : nullptr));
    });
    host.RegisterNative("Destruction", "Shatter", [](NativeCall& c) {
        if (!c.IsEntity(0)) {
            c.Fail("Destruction.Shatter(entity [, point]) expected");
            return;
        }
        DestructionSystem* system = DestructionSystem::Active();
        const Vec3 point = c.IsVector(1) ? c.Vector(1) : Vec3(0, 0, 0);
        c.Return(system != nullptr && system->Shatter(c.EntityArg(0), c.IsVector(1) ? &point : nullptr));
    });
    host.RegisterNative("Destruction", "Debris", [](NativeCall& c) {
        const DestructionSystem* system = DestructionSystem::Active();
        c.Return(static_cast<f64>(system ? system->DebrisCount() : 0));
    });
}

#else

void InstallDestructionApi(LuauHost&) {}

#endif

} // namespace aether::script
