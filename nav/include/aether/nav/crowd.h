#pragma once

#include "aether/nav/components.h"

#include <memory>
#include <string>
#include <vector>

class dtCrowd;

namespace aether {

enum class NavMoveStatus : u8 { Idle, Moving, Arrived, Failed };

// A request from gameplay, Blueprints or Luau, applied by the next NavCrowd::Update.
struct NavAgentCommand {
    enum class Kind : u8 { MoveTo, MoveToEntity, Stop, Warp };
    Kind kind = Kind::MoveTo;
    Vec3 point;
    Entity target = kNullEntity;
};

// Walks its entity over the navigation mesh (Phase 20 step 3,
// docs/design/PHASE_SPECS.md §20.3): paths to a point or after another
// entity, steered with Detour's crowd (local avoidance of other agents,
// corners anticipated), turned to face where it goes. It moves the
// entity's Transform (as world space). The methods are Blueprint nodes
// and Luau methods; commands are applied on the next update.
struct NavAgent {
    f32 radius = 0.5f;
    f32 height = 2.0f;
    f32 max_speed = 3.5f;
    f32 max_acceleration = 8.0f;
    f32 stopping_distance = 0.3f; // arrived within this of the goal
    f32 base_offset = 0.0f;       // the entity sits this far above the mesh
    i32 avoidance_quality = 3;    // 0 (none) .. 3 (best)
    f32 separation_weight = 2.0f; // pushing apart from neighbours (0: none)
    bool update_rotation = true;
    f32 turn_speed = 720.0f;   // degrees per second
    bool allow_partial = false; // walk as near as it gets when the goal is cut off (else it fails)
    f32 goal_tolerance = 1.0f;  // how far the goal may be from the mesh (else the move fails, unless partial)

    // Set by the crowd; not saved.
    NavMoveStatus status = NavMoveStatus::Idle;
    Vec3 goal;      // on the mesh
    Vec3 velocity;
    f32 remaining = 0.0f; // straight-line distance to the goal
    std::vector<NavAgentCommand> commands;

    void MoveTo(const Vec3& location);
    void MoveToEntity(const Entity& target); // follows it as it moves
    void Stop();
    void Warp(const Vec3& location); // jumps there (onto the mesh)
    bool IsMoving() const { return status == NavMoveStatus::Moving; }
    bool HasArrived() const { return status == NavMoveStatus::Arrived; }
    bool HasFailed() const { return status == NavMoveStatus::Failed; }
    Vec3 GetVelocity() const { return velocity; }
    f32 GetSpeed() const { return velocity.Length(); }
    f32 GetRemainingDistance() const { return remaining; }
    Vec3 GetGoal() const { return goal; }
};

// Blueprint function library: queries on the active navigation world.
struct Navigation {
    static bool IsReachable(const Vec3& from, const Vec3& to);
    // The path's length (-1 without a complete path).
    static f32 PathLength(const Vec3& from, const Vec3& to);
    // The nearest point on the mesh (the point itself when there's none near).
    static Vec3 ProjectPoint(const Vec3& point);
    static bool IsOnNavMesh(const Vec3& point);
    // A random point reachable from `origin` within `radius` (`origin` when there's none).
    static Vec3 RandomReachablePoint(const Vec3& origin, f32 radius);
    // Where a straight walk from `from` to `to` over the mesh is stopped (`to` when it isn't).
    static Vec3 Raycast(const Vec3& from, const Vec3& to);
};

void RegisterNavAgentComponents();

} // namespace aether

namespace aether::nav {

struct MoveCompletedEvent {
    Entity entity;
    bool success = false;
};

// Runs a world's NavAgents on its NavWorld's mesh with a Detour crowd.
class NavCrowd {
public:
    static constexpr const char* kMoveCompletedEvent = "Event.OnMoveCompleted";

    NavCrowd(World& world, NavWorld& nav, i32 max_agents = 256);
    ~NavCrowd();
    NavCrowd(const NavCrowd&) = delete;
    NavCrowd& operator=(const NavCrowd&) = delete;

    // Applies commands, steps the crowd `dt` seconds and writes the transforms.
    void Update(f32 dt);
    // Move completions this update (for Blueprints' Event OnMoveCompleted).
    const std::vector<MoveCompletedEvent>& Events() const { return events_; }
    usize AgentCount() const { return agents_.size(); }
    // The corners the agent means to walk next (for debugging and the overlay).
    std::vector<Vec3> Corners(Entity e) const;
    const std::vector<std::string>& Problems() const { return problems_; }

private:
    struct Agent {
        Entity entity;
        i32 index = -1;
        Entity follow = kNullEntity;
        Vec3 follow_seen;
        Vec3 target; // what was asked for
        bool partial = false;
    };
    bool Ensure();
    void Configure(Agent& a, const NavAgent& c);
    void Request(Agent& a, NavAgent& c, const Vec3& target);
    void Finish(Agent& a, NavAgent& c, bool success);
    void Remove(u32 key);

    World& world_;
    NavWorld& nav_;
    i32 max_agents_;
    std::unique_ptr<dtCrowd, void (*)(dtCrowd*)> crowd_;
    const void* mesh_ = nullptr; // the dtNavMesh the crowd was made for
    u64 revision_ = 0;
    std::unordered_map<u32, Agent> agents_; // by entity index
    std::vector<MoveCompletedEvent> events_;
    std::vector<std::string> problems_;
};

} // namespace aether::nav

AETHER_ENUM(aether::NavMoveStatus, 1, AETHER_ENUM_VALUE(Idle), AETHER_ENUM_VALUE(Moving), AETHER_ENUM_VALUE(Arrived), AETHER_ENUM_VALUE(Failed))

AETHER_REFLECT(aether::NavAgent, 1,
    AETHER_FIELD(radius, Field_EditAnywhere, {.range_min = 0.05, .range_max = 4.0, .units = "m"}),
    AETHER_FIELD(height, Field_EditAnywhere, {.range_min = 0.1, .range_max = 20.0, .units = "m"}),
    AETHER_FIELD(max_speed, Field_EditAnywhere, {.range_min = 0.0, .range_max = 100.0, .units = "m/s"}),
    AETHER_FIELD(max_acceleration, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1000.0, .units = "m/s2"}),
    AETHER_FIELD(stopping_distance, Field_EditAnywhere, {.tooltip = "Arrived within this of the goal", .range_min = 0.0, .range_max = 100.0, .units = "m"}),
    AETHER_FIELD(base_offset, Field_EditAnywhere, {.tooltip = "Height of the entity above the mesh", .range_min = -10.0, .range_max = 10.0, .units = "m"}),
    AETHER_FIELD(avoidance_quality, Field_EditAnywhere, {.tooltip = "Avoiding other agents: 0 none .. 3 best", .range_min = 0.0, .range_max = 3.0}),
    AETHER_FIELD(separation_weight, Field_EditAnywhere, {.range_min = 0.0, .range_max = 20.0}),
    AETHER_FIELD(update_rotation, Field_EditAnywhere, {.tooltip = "Turn to face where it's going"}),
    AETHER_FIELD(turn_speed, Field_EditAnywhere, {.range_min = 0.0, .range_max = 10000.0, .units = "deg/s"}),
    AETHER_FIELD(allow_partial, Field_EditAnywhere, {.tooltip = "Walk as near as it gets when the goal can't be reached"}),
    AETHER_FIELD(goal_tolerance, Field_EditAnywhere, {.tooltip = "How far off the mesh a goal may be", .range_min = 0.0, .range_max = 100.0, .units = "m"}),
    AETHER_FIELD(status, Field_ReadOnly | Field_Transient),
    AETHER_METHOD(MoveTo, Fn_BlueprintCallable, {"location"}),
    AETHER_METHOD(MoveToEntity, Fn_BlueprintCallable, {"target"}),
    AETHER_METHOD(Stop, Fn_BlueprintCallable),
    AETHER_METHOD(Warp, Fn_BlueprintCallable, {"location"}),
    AETHER_METHOD(IsMoving, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(HasArrived, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(HasFailed, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(GetVelocity, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(GetSpeed, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(GetRemainingDistance, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(GetGoal, Fn_BlueprintCallable | Fn_Pure)
)

AETHER_REFLECT(aether::Navigation, 1,
    AETHER_METHOD(IsReachable, Fn_BlueprintCallable | Fn_Pure, {"from", "to"}),
    AETHER_METHOD(PathLength, Fn_BlueprintCallable | Fn_Pure, {"from", "to"}),
    AETHER_METHOD(ProjectPoint, Fn_BlueprintCallable | Fn_Pure, {"point"}),
    AETHER_METHOD(IsOnNavMesh, Fn_BlueprintCallable | Fn_Pure, {"point"}),
    AETHER_METHOD(RandomReachablePoint, Fn_BlueprintCallable, {"origin", "radius"}),
    AETHER_METHOD(Raycast, Fn_BlueprintCallable | Fn_Pure, {"from", "to"})
)
