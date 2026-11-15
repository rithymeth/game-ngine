#include "aether/script/achievements_api.h"

#if AETHER_KIT_ACHIEVEMENTS
#include "aether/achievements/achievement_system.h"
#endif

namespace aether::script {

#if AETHER_KIT_ACHIEVEMENTS

namespace {

using namespace aether::ach;

bool NeedName(NativeCall& c) {
    if (c.IsString(0)) return true;
    c.Fail("Achievements: the achievement must be a name (a string)");
    return false;
}

const AchievementDef* Def(const std::string& name) {
    const AchievementSystem* system = AchievementSystem::Active();
    return system ? system->Library().Find(name) : nullptr;
}

} // namespace

void InstallAchievementsApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("Achievements", name, std::move(fn)); };
    def("Add", [](NativeCall& c) {
        if (!NeedName(c)) return;
        AchievementSystem* system = AchievementSystem::Active();
        f64 amount = c.IsNumber(1) ? c.Number(1) : 1.0;
        const i32 whole = amount >= 1.0 && amount <= AchievementDef::kMaxTarget ? static_cast<i32>(amount) : 0; // 0 is refused as a bad amount
        c.Return(std::string(ResultName(system ? system->Add(c.String(0), whole) : Result::Unknown)));
    });
    def("Unlock", [](NativeCall& c) {
        if (!NeedName(c)) return;
        AchievementSystem* system = AchievementSystem::Active();
        c.Return(std::string(ResultName(system ? system->Unlock(c.String(0)) : Result::Unknown)));
    });
    def("Reset", [](NativeCall& c) {
        if (!NeedName(c)) return;
        AchievementSystem* system = AchievementSystem::Active();
        c.Return(std::string(ResultName(system ? system->Reset(c.String(0)) : Result::Unknown)));
    });
    def("IsUnlocked", [](NativeCall& c) {
        if (!NeedName(c)) return;
        const AchievementSystem* system = AchievementSystem::Active();
        c.Return(system != nullptr && system->IsUnlocked(c.String(0)));
    });
    def("GetProgress", [](NativeCall& c) {
        if (!NeedName(c)) return;
        const AchievementSystem* system = AchievementSystem::Active();
        c.Return(static_cast<f64>(system ? system->Progress(c.String(0)) : 0));
    });
    def("GetTarget", [](NativeCall& c) {
        if (!NeedName(c)) return;
        const AchievementDef* a = Def(c.String(0));
        c.Return(static_cast<f64>(a ? a->target : 0));
    });
    def("GetTitle", [](NativeCall& c) {
        if (!NeedName(c)) return;
        const AchievementDef* a = Def(c.String(0));
        c.Return(a ? (a->title.empty() ? a->name : a->title) : std::string());
    });
    def("GetDescription", [](NativeCall& c) {
        if (!NeedName(c)) return;
        const AchievementDef* a = Def(c.String(0));
        c.Return(a ? a->description : std::string());
    });
    def("Exists", [](NativeCall& c) {
        if (!NeedName(c)) return;
        c.Return(Def(c.String(0)) != nullptr);
    });
    def("IsHidden", [](NativeCall& c) {
        if (!NeedName(c)) return;
        const AchievementDef* a = Def(c.String(0));
        c.Return(a != nullptr && a->hidden);
    });
    def("UnlockedCount", [](NativeCall& c) {
        const AchievementSystem* system = AchievementSystem::Active();
        c.Return(static_cast<f64>(system ? system->UnlockedCount() : 0));
    });
    def("NextUnlocked", [](NativeCall& c) {
        AchievementSystem* system = AchievementSystem::Active();
        c.Return(system ? system->NextUnlocked() : std::string());
    });
}

#else

void InstallAchievementsApi(LuauHost&) {}

#endif

} // namespace aether::script
