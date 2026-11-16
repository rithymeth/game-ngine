#include "aether/script/ropes_api.h"

#if AETHER_KIT_ROPES
#include "aether/ropes/rope_system.h"
#endif

namespace aether::script {

#if AETHER_KIT_ROPES

void InstallRopesApi(LuauHost& host) {
    using aether::ropes::RopeSystem;
    host.RegisterNative("Ropes", "Cut", [](NativeCall& c) {
        if (!c.IsEntity(0) || !c.IsNumber(1)) {
            c.Fail("Ropes.Cut(entity, fraction) expected");
            return;
        }
        RopeSystem* system = RopeSystem::Active();
        c.Return(system != nullptr && system->Cut(c.EntityArg(0), static_cast<f32>(c.Number(1))));
    });
    host.RegisterNative("Ropes", "Length", [](NativeCall& c) {
        if (!c.IsEntity(0)) {
            c.Fail("Ropes.Length(entity) expected");
            return;
        }
        const RopeSystem* system = RopeSystem::Active();
        c.Return(static_cast<f64>(system != nullptr ? system->CurrentLength(c.EntityArg(0)) : 0.0f));
    });
    host.RegisterNative("Ropes", "Segments", [](NativeCall& c) {
        if (!c.IsEntity(0)) {
            c.Fail("Ropes.Segments(entity) expected");
            return;
        }
        const RopeSystem* system = RopeSystem::Active();
        c.Return(static_cast<f64>(system != nullptr ? system->Segments(c.EntityArg(0)).size() : 0));
    });
    host.RegisterNative("Ropes", "SegmentPosition", [](NativeCall& c) {
        if (!c.IsEntity(0) || !c.IsNumber(1)) {
            c.Fail("Ropes.SegmentPosition(entity, index) expected");
            return;
        }
        const RopeSystem* system = RopeSystem::Active();
        Vec3 position(0, 0, 0);
        const f64 index = c.Number(1);
        if (system != nullptr && index >= 0.0 && index < 4294967295.0) system->SegmentPosition(c.EntityArg(0), static_cast<u32>(index), position);
        c.ReturnVector(position);
    });
}

#else

void InstallRopesApi(LuauHost&) {}

#endif

} // namespace aether::script
