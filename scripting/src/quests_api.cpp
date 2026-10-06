#include "aether/script/quests_api.h"

#if AETHER_KIT_QUESTS
#include "aether/quests/quest_library.h"
#endif

namespace aether::script {

#if AETHER_KIT_QUESTS

namespace {

using namespace aether::quest;

bool NeedEntity(NativeCall& c) {
    if (c.IsEntity(0)) return true;
    c.Fail("Quests: the owner must be an entity");
    return false;
}
bool NeedString(NativeCall& c, usize i, const char* what) {
    if (c.IsString(i)) return true;
    c.Fail(std::string("Quests: ") + what + " must be a string");
    return false;
}

} // namespace

void InstallQuestsApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("Quests", name, std::move(fn)); };
    using QuestFn = bool (*)(const Entity&, const std::string&);
    const auto quest_fn = [&](const char* name, QuestFn fn) {
        def(name, [fn](NativeCall& c) {
            if (NeedEntity(c) && NeedString(c, 1, "the quest")) c.Return(fn(c.EntityArg(0), c.String(1)));
        });
    };
    quest_fn("Start", &Quests::StartQuest);
    quest_fn("Abandon", &Quests::AbandonQuest);
    quest_fn("Fail", &Quests::FailQuest);
    quest_fn("IsActive", &Quests::IsQuestActive);
    quest_fn("IsCompleted", &Quests::IsQuestCompleted);
    def("Progress", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the quest") && NeedString(c, 2, "the objective")) c.Return(Quests::ProgressQuest(c.EntityArg(0), c.String(1), c.String(2), c.IsNumber(3) ? static_cast<i32>(c.Number(3)) : 1));
    });
    def("Notify", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the kind") && NeedString(c, 2, "the target")) c.Return(static_cast<f64>(Quests::NotifyQuests(c.EntityArg(0), c.String(1), c.String(2), c.IsNumber(3) ? static_cast<i32>(c.Number(3)) : 1)));
    });
    def("GetState", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the quest")) c.Return(Quests::GetQuestState(c.EntityArg(0), c.String(1)));
    });
    def("GetProgress", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the quest") && NeedString(c, 2, "the objective")) c.Return(static_cast<f64>(Quests::GetQuestProgress(c.EntityArg(0), c.String(1), c.String(2))));
    });
    def("GetTitle", [](NativeCall& c) {
        if (NeedString(c, 0, "the quest")) c.Return(Quests::GetQuestTitle(c.String(0)));
    });
}

#else

void InstallQuestsApi(LuauHost&) {}

#endif

} // namespace aether::script
