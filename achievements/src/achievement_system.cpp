#include "aether/achievements/achievement_system.h"

#include <algorithm>

namespace aether::ach {

namespace {
AchievementSystem* g_active = nullptr;
} // namespace

const char* ResultName(Result result) {
    switch (result) {
        case Result::Ok: return "ok";
        case Result::Unknown: return "unknown_achievement";
        case Result::BadAmount: return "bad_amount";
    }
    return "unknown";
}

AchievementSystem::AchievementSystem(save::SaveBag& bag, const AchievementLibrary& library) : bag_(bag), library_(library) { MakeActive(); }
AchievementSystem::~AchievementSystem() {
    if (g_active == this) g_active = nullptr;
}
AchievementSystem* AchievementSystem::Active() { return g_active; }
void AchievementSystem::MakeActive() { g_active = this; }

i32 AchievementSystem::Progress(const std::string& name) const {
    const AchievementDef* def = library_.Find(name);
    if (def == nullptr) return 0;
    return std::clamp(bag_.GetInt(Key(name), 0), 0, def->target); // a bag edited by hand or by another version never reads past its range
}

bool AchievementSystem::IsUnlocked(const std::string& name) const {
    const AchievementDef* def = library_.Find(name);
    return def != nullptr && Progress(name) >= def->target;
}

usize AchievementSystem::UnlockedCount() const {
    usize count = 0;
    for (const std::string& name : library_.Names()) count += IsUnlocked(name) ? 1 : 0;
    return count;
}

Result AchievementSystem::Add(const std::string& name, i32 amount) {
    const AchievementDef* def = library_.Find(name);
    if (def == nullptr) return Result::Unknown;
    if (amount < 1 || amount > AchievementDef::kMaxTarget) return Result::BadAmount;
    const i32 before = Progress(name);
    if (before >= def->target) return Result::Ok; // done: nothing more to count
    const i32 after = static_cast<i32>(std::min<i64>(static_cast<i64>(before) + amount, def->target));
    bag_.SetInt(Key(name), after);
    if (after >= def->target) unlocked_.push_back(name);
    return Result::Ok;
}

Result AchievementSystem::Unlock(const std::string& name) {
    const AchievementDef* def = library_.Find(name);
    if (def == nullptr) return Result::Unknown;
    if (Progress(name) >= def->target) return Result::Ok;
    bag_.SetInt(Key(name), def->target);
    unlocked_.push_back(name);
    return Result::Ok;
}

Result AchievementSystem::Reset(const std::string& name) {
    if (library_.Find(name) == nullptr) return Result::Unknown;
    bag_.Remove(Key(name));
    unlocked_.erase(std::remove(unlocked_.begin(), unlocked_.end(), name), unlocked_.end());
    return Result::Ok;
}

std::string AchievementSystem::NextUnlocked() {
    if (unlocked_.empty()) return {};
    std::string next = std::move(unlocked_.front());
    unlocked_.erase(unlocked_.begin());
    return next;
}

std::vector<std::string> AchievementSystem::TakeUnlocked() {
    std::vector<std::string> taken;
    taken.swap(unlocked_);
    return taken;
}

} // namespace aether::ach
