#include "aether/script/dialogue_api.h"

#if AETHER_KIT_DIALOGUE
#include "aether/dialogue/dialogue_system.h"
#endif

namespace aether::script {

#if AETHER_KIT_DIALOGUE

namespace {

using namespace aether::dlg;

bool NeedEntity(NativeCall& c) {
    if (c.IsEntity(0)) return true;
    c.Fail("Dialogue: the listener must be an entity");
    return false;
}
bool NeedString(NativeCall& c, usize i, const char* what) {
    if (c.IsString(i)) return true;
    c.Fail(std::string("Dialogue: ") + what + " must be a string");
    return false;
}
bool NeedNumber(NativeCall& c, usize i, const char* what) {
    if (c.IsNumber(i)) return true;
    c.Fail(std::string("Dialogue: ") + what + " must be a number");
    return false;
}

} // namespace

void InstallDialogueApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("Dialogue", name, std::move(fn)); };
    def("Start", [](NativeCall& c) {
        if (!NeedEntity(c) || !NeedString(c, 1, "the dialogue")) return;
        DialogueSystem* system = DialogueSystem::Active();
        c.Return(std::string(ResultName(system ? system->Start(c.EntityArg(0), c.String(1)) : Result::UnknownDialogue)));
    });
    def("IsActive", [](NativeCall& c) {
        if (!NeedEntity(c)) return;
        const DialogueSystem* system = DialogueSystem::Active();
        c.Return(system != nullptr && system->IsActive(c.EntityArg(0)));
    });
    const auto text_of = [&](const char* name, std::string Line::*field) {
        def(name, [field](NativeCall& c) {
            if (!NeedEntity(c)) return;
            const DialogueSystem* system = DialogueSystem::Active();
            const std::optional<Line> line = system ? system->Current(c.EntityArg(0)) : std::nullopt;
            c.Return(line ? (*line).*field : std::string());
        });
    };
    text_of("Speaker", &Line::speaker);
    text_of("Text", &Line::text);
    text_of("Event", &Line::event);
    def("ChoiceCount", [](NativeCall& c) {
        if (!NeedEntity(c)) return;
        const DialogueSystem* system = DialogueSystem::Active();
        const std::optional<Line> line = system ? system->Current(c.EntityArg(0)) : std::nullopt;
        c.Return(static_cast<f64>(line ? line->choices.size() : 0));
    });
    def("Choice", [](NativeCall& c) {
        if (!NeedEntity(c) || !NeedNumber(c, 1, "the choice number")) return;
        const DialogueSystem* system = DialogueSystem::Active();
        const std::optional<Line> line = system ? system->Current(c.EntityArg(0)) : std::nullopt;
        const f64 n = c.Number(1);
        c.Return(line && n >= 1 && n <= static_cast<f64>(line->choices.size()) ? line->choices[static_cast<usize>(n) - 1] : std::string());
    });
    def("IsLast", [](NativeCall& c) {
        if (!NeedEntity(c)) return;
        const DialogueSystem* system = DialogueSystem::Active();
        const std::optional<Line> line = system ? system->Current(c.EntityArg(0)) : std::nullopt;
        c.Return(line.has_value() && line->last);
    });
    def("Advance", [](NativeCall& c) {
        if (!NeedEntity(c)) return;
        DialogueSystem* system = DialogueSystem::Active();
        c.Return(std::string(ResultName(system ? system->Advance(c.EntityArg(0)) : Result::NotActive)));
    });
    def("Choose", [](NativeCall& c) {
        if (!NeedEntity(c) || !NeedNumber(c, 1, "the choice number")) return;
        DialogueSystem* system = DialogueSystem::Active();
        const f64 n = c.Number(1);
        const i32 index = n >= 1 && n < 100000 ? static_cast<i32>(n) - 1 : -1; // Lua counts from 1; anything else is no choice
        c.Return(std::string(ResultName(system ? system->Choose(c.EntityArg(0), index) : Result::NotActive)));
    });
    def("End", [](NativeCall& c) {
        if (!NeedEntity(c)) return;
        if (DialogueSystem* system = DialogueSystem::Active()) system->End(c.EntityArg(0));
    });
    def("Exists", [](NativeCall& c) {
        if (!NeedString(c, 0, "the dialogue")) return;
        const DialogueSystem* system = DialogueSystem::Active();
        c.Return(system != nullptr && system->Dialogues().Find(c.String(0)) != nullptr);
    });
}

#else

void InstallDialogueApi(LuauHost&) {}

#endif

} // namespace aether::script
