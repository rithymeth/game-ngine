#include "aether/script/interaction_api.h"

#if AETHER_KIT_INTERACTION
#include "aether/interaction/interaction_library.h"
#endif

namespace aether::script {

#if AETHER_KIT_INTERACTION

namespace {

using namespace aether::interact;

bool NeedEntity(NativeCall& c, usize i, const char* what) {
    if (c.IsEntity(i)) return true;
    c.Fail(std::string("Interaction: ") + what + " must be an entity");
    return false;
}

} // namespace

void InstallInteractionApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("Interaction", name, std::move(fn)); };
    def("Find", [](NativeCall& c) {
        if (!NeedEntity(c, 0, "the interactor")) return;
        for (usize i = 1; i <= 6; ++i) {
            if (!c.IsNumber(i)) {
                c.Fail("Interaction: Find takes the interactor, a position (x, y, z) and a direction (x, y, z)");
                return;
            }
        }
        const Vec3 from(static_cast<f32>(c.Number(1)), static_cast<f32>(c.Number(2)), static_cast<f32>(c.Number(3)));
        const Vec3 forward(static_cast<f32>(c.Number(4)), static_cast<f32>(c.Number(5)), static_cast<f32>(c.Number(6)));
        const Entity found = Interaction::FindInteractable(c.EntityArg(0), from, forward);
        if (found.IsNull()) c.Return(ScriptValue{});
        else c.Return(ScriptValue{EntityRef{found}});
    });
    def("GetPrompt", [](NativeCall& c) {
        if (NeedEntity(c, 0, "the target")) c.Return(Interaction::GetInteractionPrompt(c.EntityArg(0)));
    });
    def("CanInteract", [](NativeCall& c) {
        if (NeedEntity(c, 0, "the interactor") && NeedEntity(c, 1, "the target")) c.Return(Interaction::CanInteract(c.EntityArg(0), c.EntityArg(1)));
    });
    def("Interact", [](NativeCall& c) {
        if (NeedEntity(c, 0, "the interactor") && NeedEntity(c, 1, "the target")) c.Return(Interaction::Interact(c.EntityArg(0), c.EntityArg(1)));
    });
    def("SetEnabled", [](NativeCall& c) {
        if (!NeedEntity(c, 0, "the target")) return;
        if (!c.IsBool(1)) {
            c.Fail("Interaction: enabled must be true or false");
            return;
        }
        c.Return(Interaction::SetInteractable(c.EntityArg(0), c.Bool(1)));
    });
    def("Reset", [](NativeCall& c) {
        if (NeedEntity(c, 0, "the target")) c.Return(Interaction::ResetInteractable(c.EntityArg(0)));
    });
}

#else

void InstallInteractionApi(LuauHost&) {}

#endif

} // namespace aether::script
