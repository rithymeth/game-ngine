#pragma once

#include "aether/physics/physics_scene.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace aether {

// Character movement (Phase 13 step 5, docs/design/PHASE_SPECS.md §13.3):
// a capsule moved by Jolt's CharacterVirtual. It walks, runs, climbs steps
// and slopes up to a limit, slides down steeper ones, jumps (with coyote
// time and a jump buffer), and rides moving platforms. The Transform's
// position is the character's feet.

enum class MovementMode : u8 { Walking, Falling, Flying, Swimming };

struct CharacterMovement {
    // Settings (saved; defaults from the spec).
    f32 radius = 0.35f;
    f32 height = 1.8f; // the whole capsule
    f32 walk_speed = 4.0f;
    f32 run_speed = 7.0f;
    f32 acceleration = 20.0f;    // on the ground, toward the input's speed
    f32 braking = 25.0f;         // on the ground, with no input
    f32 air_acceleration = 5.0f; // in the air
    f32 air_control = 0.3f;      // in the air, how much of the input counts (0..1)
    f32 jump_velocity = 5.0f;
    f32 coyote_time = 0.1f;      // jumping still works this long after walking off a ledge
    f32 jump_buffer = 0.1f;      // a jump pressed this long before landing still happens
    f32 max_slope_degrees = 45.0f;
    f32 step_height = 0.35f;
    f32 gravity_scale = 1.0f;
    f32 mass = 70.0f;            // for pushing dynamic bodies
    bool flying = false;         // no gravity; input moves in 3D

    // Input for the next step (consumed by it).
    Vec3 input{0, 0, 0}; // world-space direction; lengths over 1 are clamped
    bool run = false;
    bool jump_requested = false;
    void AddInput(const Vec3& direction) { input = input + direction; }
    void Jump() { jump_requested = true; }
    // Root motion for the next step (Phase 16 step 4): a world-space
    // displacement that replaces the input's horizontal movement (gravity
    // and jumps still apply), and a turn about +Y applied to the Transform.
    void AddRootMotion(const Vec3& displacement, f32 yaw_radians) {
        root_motion = root_motion + displacement;
        root_motion_yaw += yaw_radians;
        has_root_motion = true;
    }
    Vec3 root_motion{0, 0, 0};
    f32 root_motion_yaw = 0.0f;
    bool has_root_motion = false;

    // State (read-only for gameplay).
    Vec3 velocity{0, 0, 0};
    Vec3 move_velocity{0, 0, 0}; // the character's own, relative to what it stands on
    MovementMode mode = MovementMode::Falling;
    bool grounded = false;
    Vec3 ground_normal{0, 1, 0};
    f32 coyote_left = 0.0f;
    f32 jump_buffer_left = 0.0f;
};

enum class CharacterEventType : u8 { Landed, Jumped, MovementModeChanged };
struct CharacterEvent {
    Entity entity;
    CharacterEventType type = CharacterEventType::Landed;
    MovementMode from = MovementMode::Falling, to = MovementMode::Falling; // mode changes
    f32 impact_speed = 0.0f;                                               // landings: downward speed, m/s
};
// "Event.OnLanded", "Event.OnJumped", "Event.OnMovementModeChanged".
const char* CharacterEventName(CharacterEventType type);

class CharacterSystem {
public:
    // With a PhysicsScene, each character's inner body is registered with it,
    // so triggers, contact events and queries name the character's entity.
    CharacterSystem(World& world, PhysicsWorld& physics, PhysicsScene* scene = nullptr);
    ~CharacterSystem();

    CharacterSystem(const CharacterSystem&) = delete;
    CharacterSystem& operator=(const CharacterSystem&) = delete;

    // One fixed step for every entity with CharacterMovement and Transform:
    // creates characters for new ones (and removes the ones gone), moves
    // them, writes their Transforms, and reports events. Run it before the
    // PhysicsScene's step.
    void Step(f32 dt);

    using EventHandler = std::function<void(const CharacterEvent&)>;
    void SetEventHandler(EventHandler handler) { handler_ = std::move(handler); }
    const std::vector<CharacterEvent>& Events() const { return events_; } // the last step's

    usize Count() const { return characters_.size(); }
    JPH::BodyID InnerBodyOf(Entity entity) const;

private:
    struct Tracked;
    void Report(const CharacterEvent& e);

    World& world_;
    PhysicsWorld& physics_;
    PhysicsScene* scene_;
    std::unordered_map<u64, std::unique_ptr<Tracked>> characters_;
    EventHandler handler_;
    std::vector<CharacterEvent> events_;
};

} // namespace aether

AETHER_ENUM(aether::MovementMode, 1, AETHER_ENUM_VALUE(Walking), AETHER_ENUM_VALUE(Falling), AETHER_ENUM_VALUE(Flying),
            AETHER_ENUM_VALUE(Swimming))

AETHER_REFLECT(aether::CharacterMovement, 1,
    AETHER_FIELD(radius, Field_EditAnywhere, {.category = "Shape", .range_min = 0.05, .range_max = 5.0, .units = "m"}),
    AETHER_FIELD(height, Field_EditAnywhere, {.tooltip = "The whole capsule", .category = "Shape", .range_min = 0.1, .range_max = 20.0, .units = "m"}),
    AETHER_FIELD(walk_speed, Field_EditAnywhere, {.category = "Movement", .range_min = 0.0, .range_max = 100.0, .units = "m/s"}),
    AETHER_FIELD(run_speed, Field_EditAnywhere, {.category = "Movement", .range_min = 0.0, .range_max = 100.0, .units = "m/s"}),
    AETHER_FIELD(acceleration, Field_EditAnywhere, {.category = "Movement", .range_min = 0.0, .range_max = 1000.0, .units = "m/s2"}),
    AETHER_FIELD(braking, Field_EditAnywhere, {.category = "Movement", .range_min = 0.0, .range_max = 1000.0, .units = "m/s2"}),
    AETHER_FIELD(air_acceleration, Field_EditAnywhere, {.category = "Movement", .range_min = 0.0, .range_max = 1000.0, .units = "m/s2"}),
    AETHER_FIELD(air_control, Field_EditAnywhere, {.tooltip = "How much of the input counts in the air", .category = "Movement", .range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(jump_velocity, Field_EditAnywhere, {.category = "Jumping", .range_min = 0.0, .range_max = 100.0, .units = "m/s"}),
    AETHER_FIELD(coyote_time, Field_EditAnywhere, {.tooltip = "Jumping still works this long after leaving the ground", .category = "Jumping", .range_min = 0.0, .range_max = 1.0, .units = "s"}),
    AETHER_FIELD(jump_buffer, Field_EditAnywhere, {.tooltip = "A jump pressed this long before landing still happens", .category = "Jumping", .range_min = 0.0, .range_max = 1.0, .units = "s"}),
    AETHER_FIELD(max_slope_degrees, Field_EditAnywhere, {.category = "Movement", .range_min = 0.0, .range_max = 89.0, .units = "deg"}),
    AETHER_FIELD(step_height, Field_EditAnywhere, {.category = "Movement", .range_min = 0.0, .range_max = 2.0, .units = "m"}),
    AETHER_FIELD(gravity_scale, Field_EditAnywhere, {.category = "Movement", .range_min = -10.0, .range_max = 10.0}),
    AETHER_FIELD(mass, Field_EditAnywhere, {.tooltip = "For pushing dynamic bodies", .category = "Movement", .range_min = 1.0, .range_max = 10000.0, .units = "kg"}),
    AETHER_FIELD(flying, Field_EditAnywhere, {.tooltip = "No gravity; input moves in 3D", .category = "Movement"}),
    AETHER_FIELD(input, Field_Transient),
    AETHER_FIELD(jump_requested, Field_Transient)
)
