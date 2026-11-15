#pragma once

#include "aether/achievements/achievement_def.h"
#include "aether/save/save_bag.h"

#include <string>
#include <vector>

namespace aether::ach {

enum class Result : u8 { Ok, Unknown, BadAmount };
// "ok", "unknown_achievement", "bad_amount" (an amount is 1 to AchievementDef::kMaxTarget)
const char* ResultName(Result result);

// Tracks progress toward achievements. The progress is not kept here: it lives in the game's save bag, as an integer under "achievement.<name>",
// so it is written and read with the save slot and a scene change does not touch it. Reaching the target unlocks the achievement; the unlock is
// remembered for TakeUnlocked (the toast the game shows) once. Deterministic and event-driven (no Update).
class AchievementSystem {
public:
    AchievementSystem(save::SaveBag& bag, const AchievementLibrary& library);
    ~AchievementSystem();
    AchievementSystem(const AchievementSystem&) = delete;
    AchievementSystem& operator=(const AchievementSystem&) = delete;

    static AchievementSystem* Active();
    void MakeActive();

    // Adds `amount` (1 to kMaxTarget) of progress, up to the target. Unlocks (and queues the toast) the moment it reaches it.
    Result Add(const std::string& name, i32 amount = 1);
    // Sets the progress to the target at once. Does nothing (Ok) if it was already unlocked.
    Result Unlock(const std::string& name);
    // Back to no progress (a game's new-game button; the save bag holds nothing else of it).
    Result Reset(const std::string& name);

    i32 Progress(const std::string& name) const; // 0 for an unknown achievement
    bool IsUnlocked(const std::string& name) const;
    usize UnlockedCount() const;
    // The achievements unlocked since the last call, in the order they were, each once.
    std::vector<std::string> TakeUnlocked();
    // One of them at a time: the oldest not yet taken, or "" when there is none.
    std::string NextUnlocked();

    const AchievementLibrary& Library() const { return library_; }

    static std::string Key(const std::string& name) { return "achievement." + name; }

private:
    save::SaveBag& bag_;
    const AchievementLibrary& library_;
    std::vector<std::string> unlocked_;
};

} // namespace aether::ach
