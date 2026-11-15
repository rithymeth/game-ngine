#include "aether/script/spawner_api.h"

#if AETHER_KIT_SPAWNER
#include "aether/ecs/world.h"
#include "aether/spawner/spawner_system.h"
#endif

namespace aether::script {

#if AETHER_KIT_SPAWNER

namespace {

using namespace aether::spawn;

bool NeedEntity(NativeCall& c) {
    if (c.IsEntity(0)) return true;
    c.Fail("Spawner: the spawner must be an entity");
    return false;
}

} // namespace

void InstallSpawnerApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("Spawner", name, std::move(fn)); };
    def("SpawnNow", [](NativeCall& c) {
        if (!NeedEntity(c)) return;
        SpawnerSystem* system = SpawnerSystem::Active();
        const f64 count = c.IsNumber(1) ? c.Number(1) : 1.0;
        const i32 whole = count >= 1.0 && count <= Spawner::kMaxBurst ? static_cast<i32>(count) : 0; // 0 is refused
        c.Return(static_cast<f64>(system ? system->SpawnNow(c.EntityArg(0), whole) : 0));
    });
    def("AliveCount", [](NativeCall& c) {
        if (!NeedEntity(c)) return;
        SpawnerSystem* system = SpawnerSystem::Active();
        c.Return(static_cast<f64>(system ? system->AliveCount(c.EntityArg(0)) : 0));
    });
    def("TotalSpawned", [](NativeCall& c) {
        if (!NeedEntity(c)) return;
        const SpawnerSystem* system = SpawnerSystem::Active();
        c.Return(static_cast<f64>(system ? system->TotalSpawned(c.EntityArg(0)) : 0));
    });
}

#else

void InstallSpawnerApi(LuauHost&) {}

#endif

} // namespace aether::script
