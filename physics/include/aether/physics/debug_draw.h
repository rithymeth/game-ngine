#pragma once

#include "aether/physics/character.h"

#include <vector>

namespace aether {

// Physics debug drawing and collider gizmos (Phase 13 step 6). Both produce
// plain data, world-space lines and handle positions, so they're tested
// headless; the editor viewport (and a game's debug overlay) draws them.

struct DebugLine {
    Vec3 a, b;
    u32 color = 0xFFFFFFFF; // ImGui order (ABGR)
};

// The viewport menu's physics toggle. Saved with the editor settings.
struct PhysicsDebugOptions {
    bool enabled = false;
    bool colliders = true;
    bool triggers = true;
    bool characters = true;
    bool contacts = false; // Begin/Stay points of the last step, with their normals
    bool dim_sleeping = true;
};

// Colors: static gray, kinematic blue, dynamic green (dimmer asleep),
// triggers orange, characters cyan, contacts red.
namespace debug_colors {
inline constexpr u32 kStatic = 0xFFA0A0A0;
inline constexpr u32 kKinematic = 0xFFFF9040;
inline constexpr u32 kDynamic = 0xFF40E040;
inline constexpr u32 kSleeping = 0xFF208020;
inline constexpr u32 kTrigger = 0xFF20A0FF;
inline constexpr u32 kCharacter = 0xFFFFE040;
inline constexpr u32 kContact = 0xFF3030FF;
inline constexpr u32 kSelected = 0xFF00FFFF;
} // namespace debug_colors

// Appends the lines for everything `options` asks for. Boxes, spheres and
// capsules are drawn exactly; convex and mesh colliders from their bodies'
// triangles (so they need a PhysicsScene body; without one, a cross per
// point). `scene` may be null. Characters are drawn from their components.
void DrawPhysicsDebug(const World& world, const PhysicsScene* scene, const PhysicsDebugOptions& options, std::vector<DebugLine>& out);
// One entity's colliders (the selected one's gizmo), in `color`.
void DrawColliderWireframe(const World& world, const PhysicsScene* scene, Entity entity, u32 color, std::vector<DebugLine>& out);

// --- Gizmo handles ---------------------------------------------------------------
// Box: one per face, moving that face (the opposite one stays put). Sphere:
// six on the surface, changing the radius. Capsule: four around the middle
// for the radius, top and bottom for the height.
enum class ColliderHandleKind : u8 { BoxFace, SphereRadius, CapsuleRadius, CapsuleHeight };
struct ColliderHandle {
    ColliderHandleKind kind = ColliderHandleKind::BoxFace;
    u8 axis = 0;    // local 0 = x, 1 = y, 2 = z
    i8 sign = 1;    // +1 or -1
    Vec3 position;  // world space
    Vec3 direction; // world space, unit: dragging along it grows the collider
};
std::vector<ColliderHandle> ColliderHandles(const World& world, Entity entity);
// Applies a drag of `distance` along the handle's direction (negative
// shrinks) to the entity's collider. Sizes don't go below 1 cm. False if the
// entity has no such collider.
bool DragColliderHandle(World& world, Entity entity, const ColliderHandle& handle, f32 distance);

} // namespace aether

AETHER_REFLECT(aether::PhysicsDebugOptions, 1,
    AETHER_FIELD(enabled, Field_EditAnywhere, {.tooltip = "Draw physics shapes in the viewport"}),
    AETHER_FIELD(colliders, Field_EditAnywhere),
    AETHER_FIELD(triggers, Field_EditAnywhere),
    AETHER_FIELD(characters, Field_EditAnywhere),
    AETHER_FIELD(contacts, Field_EditAnywhere, {.tooltip = "Contact points of the last step"}),
    AETHER_FIELD(dim_sleeping, Field_EditAnywhere, {.tooltip = "Draw sleeping bodies darker"})
)
