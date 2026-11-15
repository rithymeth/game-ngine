#include "aether/script/checkpoints_api.h"

#if AETHER_KIT_CHECKPOINTS
#include "aether/checkpoints/checkpoint_system.h"
#endif

namespace aether::script {

#if AETHER_KIT_CHECKPOINTS

namespace {

using namespace aether::chk;

bool NeedName(NativeCall& c) {
    if (c.IsString(0)) return true;
    c.Fail("Checkpoints: the checkpoint must be a name (a string)");
    return false;
}

} // namespace

void InstallCheckpointsApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("Checkpoints", name, std::move(fn)); };
    def("Last", [](NativeCall& c) {
        const CheckpointSystem* system = CheckpointSystem::Active();
        c.Return(system ? system->Last() : std::string());
    });
    def("GetScene", [](NativeCall& c) {
        const CheckpointSystem* system = CheckpointSystem::Active();
        c.Return(system ? system->LastScene() : std::string());
    });
    def("IsReached", [](NativeCall& c) {
        if (!NeedName(c)) return;
        const CheckpointSystem* system = CheckpointSystem::Active();
        c.Return(system != nullptr && system->IsReached(c.String(0)));
    });
    def("Exists", [](NativeCall& c) {
        if (!NeedName(c)) return;
        const CheckpointSystem* system = CheckpointSystem::Active();
        c.Return(system != nullptr && !system->Find(c.String(0)).IsNull());
    });
    def("Activate", [](NativeCall& c) {
        if (!NeedName(c)) return;
        CheckpointSystem* system = CheckpointSystem::Active();
        c.Return(system != nullptr && system->Activate(c.String(0)));
    });
    def("Respawn", [](NativeCall& c) {
        if (!c.IsEntity(0)) {
            c.Fail("Checkpoints: the entity to respawn must be an entity");
            return;
        }
        CheckpointSystem* system = CheckpointSystem::Active();
        c.Return(std::string(ResultName(system ? system->Respawn(c.EntityArg(0)) : Result::NoCheckpoint)));
    });
}

#else

void InstallCheckpointsApi(LuauHost&) {}

#endif

} // namespace aether::script
