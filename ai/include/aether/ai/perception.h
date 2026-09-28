#pragma once

#include "aether/ecs/world.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether {

namespace nav {
class NavMesh;
}

enum class PerceptionSense : u8 { Sight, Hearing, Damage };

// What an AI knows about another entity.
struct PerceivedActor {
    Entity actor = kNullEntity;
    Vec3 location;          // where it was last sensed
    f32 age = 0.0f;         // seconds since it was last sensed
    bool visible = false;   // seen right now
    PerceptionSense last_sense = PerceptionSense::Sight;
    f32 strength = 1.0f;    // a noise's loudness or the damage done (1 for sight)
    f32 distance = 0.0f;    // from the perceiver, when last updated
};

// An AI's senses (Phase 20 step 5, docs/design/PHASE_SPECS.md §20.5):
// sight (a cone from its eyes, a radius it notices within and a larger
// one it keeps track within, blocked by walls), hearing (noises within
// its radius, scaled by their loudness) and damage (it learns who hit it).
// It forgets what it hasn't sensed for `forget_after` seconds. With
// `target_key` (and `location_key`), it writes what it knows to its
// Behavior Tree's blackboard. The methods are Blueprint nodes and Luau
// methods.
struct AIPerception {
    f32 sight_radius = 15.0f;
    f32 lose_sight_radius = 18.0f; // once seen, kept in sight out to this
    f32 fov_degrees = 90.0f;       // the whole cone, about the entity's forward (+Z)
    f32 eye_height = 1.6f;
    f32 hearing_radius = 20.0f;
    f32 forget_after = 10.0f; // seconds (0: never)
    i32 team = 0;             // 0: no team (senses everyone)
    bool detect_friends = false;
    std::string target_key;   // an Entity key: the actor it's most aware of
    std::string location_key; // a Vector key: where that actor was last sensed

    std::vector<PerceivedActor> known; // not saved

    bool CanSee(const Entity& actor) const;
    bool IsAwareOf(const Entity& actor) const;
    Vec3 GetLastKnownLocation(const Entity& actor) const;
    // What it's most aware of: the nearest it can see, else what it sensed last.
    Entity GetTarget() const;
    i32 GetKnownCount() const { return static_cast<i32>(known.size()); }
    void ForgetAll() { known.clear(); }
    const PerceivedActor* Find(Entity actor) const;
};

// Something AIs can see (hearing and damage don't need it).
struct AIStimuliSource {
    bool visible = true;
    f32 target_height = 1.0f; // where eyes aim, above its position
    i32 team = 0;
};

// Blueprint function library: stimuli for the active perception world.
struct Perception {
    // A noise at `location`, made by `instigator`, heard within each AI's hearing radius x loudness.
    static void ReportNoise(const Vec3& location, f32 loudness, const Entity& instigator);
    static void ReportDamage(const Entity& victim, const Entity& instigator, f32 amount);
};

void RegisterPerceptionComponents();

} // namespace aether

namespace aether::ai {

struct PerceptionEvent {
    Entity perceiver;
    Entity actor;
    PerceptionSense sense = PerceptionSense::Sight;
    bool sensed = true;     // false: lost sight of it
    bool forgotten = false; // it was forgotten (or went)
    Vec3 location;
};

// Runs a world's AIPerceptions.
class PerceptionWorld {
public:
    static constexpr const char* kPerceivedEvent = "Event.OnTargetPerceived"; // (actor, sense, sensed)
    static constexpr const char* kForgottenEvent = "Event.OnTargetForgotten"; // (actor)
    // Whether nothing blocks the line between two points (the two entities are the looker and the target).
    using LineOfSight = std::function<bool(const Vec3& from, const Vec3& to, Entity looker, Entity target)>;

    explicit PerceptionWorld(World& world);
    ~PerceptionWorld();
    PerceptionWorld(const PerceptionWorld&) = delete;
    PerceptionWorld& operator=(const PerceptionWorld&) = delete;

    // Without one, only distance and the cone count.
    void SetLineOfSight(LineOfSight los) { los_ = std::move(los); }
    // Walls are where the navigation mesh stops (a raycast over it from the looker's feet).
    void UseNavMeshLineOfSight(const nav::NavMesh& mesh);

    void ReportNoise(const Vec3& location, f32 loudness, Entity instigator);
    void ReportDamage(Entity victim, Entity instigator, f32 amount);

    void Update(f32 dt);
    const std::vector<PerceptionEvent>& Events() const { return events_; }
    static const char* SenseName(PerceptionSense sense);

    static PerceptionWorld* Active();
    void MakeActive();

private:
    struct Noise {
        Vec3 location;
        f32 loudness;
        Entity instigator;
    };
    struct Damage {
        Entity victim, instigator;
        f32 amount;
    };
    void Sense(Entity perceiver, AIPerception& p, Entity actor, const Vec3& at, PerceptionSense sense, f32 strength);
    void SyncBlackboard(Entity perceiver, const AIPerception& p);
    bool Hostile(const AIPerception& p, i32 team) const { return p.detect_friends || p.team == 0 || team == 0 || team != p.team; }

    World& world_;
    LineOfSight los_;
    std::vector<Noise> noises_;
    std::vector<Damage> damages_;
    std::vector<PerceptionEvent> events_;
};

} // namespace aether::ai

AETHER_ENUM(aether::PerceptionSense, 1, AETHER_ENUM_VALUE(Sight), AETHER_ENUM_VALUE(Hearing), AETHER_ENUM_VALUE(Damage))

AETHER_REFLECT(aether::AIPerception, 1,
    AETHER_FIELD(sight_radius, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(lose_sight_radius, Field_EditAnywhere, {.tooltip = "Once seen, kept in sight out to this", .range_min = 0.0, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(fov_degrees, Field_EditAnywhere, {.tooltip = "The whole view cone", .range_min = 0.0, .range_max = 360.0, .units = "deg"}),
    AETHER_FIELD(eye_height, Field_EditAnywhere, {.range_min = 0.0, .range_max = 20.0, .units = "m"}),
    AETHER_FIELD(hearing_radius, Field_EditAnywhere, {.tooltip = "Noises of loudness 1 are heard within this", .range_min = 0.0, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(forget_after, Field_EditAnywhere, {.tooltip = "Seconds without sensing before forgetting (0: never)", .range_min = 0.0, .range_max = 3600.0, .units = "s"}),
    AETHER_FIELD(team, Field_EditAnywhere, {.tooltip = "0: no team"}),
    AETHER_FIELD(detect_friends, Field_EditAnywhere, {.tooltip = "Sense its own team too"}),
    AETHER_FIELD(target_key, Field_EditAnywhere, {.tooltip = "Blackboard Entity key for the actor it's most aware of"}),
    AETHER_FIELD(location_key, Field_EditAnywhere, {.tooltip = "Blackboard Vector key for where that actor was last sensed"}),
    AETHER_METHOD(CanSee, Fn_BlueprintCallable | Fn_Pure, {"actor"}),
    AETHER_METHOD(IsAwareOf, Fn_BlueprintCallable | Fn_Pure, {"actor"}),
    AETHER_METHOD(GetLastKnownLocation, Fn_BlueprintCallable | Fn_Pure, {"actor"}),
    AETHER_METHOD(GetTarget, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(GetKnownCount, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(ForgetAll, Fn_BlueprintCallable)
)

AETHER_REFLECT(aether::AIStimuliSource, 1,
    AETHER_FIELD(visible, Field_EditAnywhere, {.tooltip = "AIs can see it"}),
    AETHER_FIELD(target_height, Field_EditAnywhere, {.tooltip = "Where eyes aim, above its position", .range_min = 0.0, .range_max = 20.0, .units = "m"}),
    AETHER_FIELD(team, Field_EditAnywhere, {.tooltip = "0: no team"})
)

AETHER_REFLECT(aether::Perception, 1,
    AETHER_METHOD(ReportNoise, Fn_BlueprintCallable, {"location", "loudness", "instigator"}),
    AETHER_METHOD(ReportDamage, Fn_BlueprintCallable, {"victim", "instigator", "amount"})
)
