#pragma once

#include "aether/core/base.h"
#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/components.h"
#include "aether/sprite2d/batch.h"

#include <cmath>
#include <utility>
#include <vector>

// 2D physics (Phase 26 step 5, docs/design/PHASE_SPECS.md §26.6): rigid
// bodies as circles and axis-aligned boxes on the entity's Transform (x and
// y; the rotation stays as it is), sequential-impulse contact resolution
// with restitution and friction, trigger volumes, collision layers, solid
// tilemap cells as static boxes, and ray and overlap queries. It is written
// for platformers and top-down games: bodies don't rotate.

namespace aether::sprite2d {

struct Vec2 {
    f32 x = 0, y = 0;
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(f32 s) const { return {x * s, y * s}; }
    f32 Dot(const Vec2& o) const { return x * o.x + y * o.y; }
    f32 Length() const { return std::sqrt(x * x + y * y); }
};

enum class BodyType2D : u8 { Static, Kinematic, Dynamic };
enum class Shape2D : u8 { Box, Circle };

// A moving body. Without one, a Collider2D is static.
struct Rigidbody2D {
    BodyType2D type = BodyType2D::Dynamic; // kinematic: moves by velocity, isn't pushed
    f32 mass = 1.0f;
    f32 gravity_scale = 1.0f;
    f32 linear_damping = 0.0f; // per second
    Vec2 velocity;
};

struct Collider2D {
    Shape2D shape = Shape2D::Box;
    Vec2 half_extents{0.5f, 0.5f}; // box
    f32 radius = 0.5f;             // circle
    Vec2 offset;                   // from the entity
    f32 friction = 0.4f;
    f32 restitution = 0.0f;
    bool trigger = false; // overlaps are reported, nothing is pushed
    u32 layer = 1;
    u32 mask = 0xFFFFFFFFu; // the layers it collides with
};

struct Physics2DSettings {
    Vec2 gravity{0.0f, -9.81f};
    u32 iterations = 6;
    f32 slop = 0.005f;       // overlap tolerated
    f32 correction = 0.8f;   // share of the overlap removed per iteration
    f32 max_step = 1.0f / 30.0f; // longer frames are cut to this
};

struct Event2D {
    enum class Kind : u8 { Begin, End, TriggerEnter, TriggerExit };
    Kind kind = Kind::Begin;
    Entity a, b; // b is null for a tile contact
    Vec2 normal; // from a toward b (a tile contact: toward the tile)
};

struct RayHit2D {
    Entity entity; // null for a tile
    Vec2 point, normal;
    f32 distance = 0;
    bool tile = false;
    i32 tile_x = 0, tile_y = 0;
};

class Physics2D {
public:
    // `tiles` supplies the tilemaps behind TilemapRenderer entities (their
    // solid cells collide); may be empty.
    explicit Physics2D(World& world, Resolvers2D tiles = {}, Physics2DSettings settings = {});

    Physics2DSettings settings;

    // Advances by `dt`: bodies are read from the world, moved, and written
    // back (Transform x, y and Rigidbody2D::velocity).
    void Step(f32 dt);

    // Contact and trigger transitions of the last Step.
    const std::vector<Event2D>& Events() const { return events_; }
    // Whether the body rested on something below it in the last Step
    // (a contact whose normal points mostly upward onto it).
    bool IsGrounded(Entity e) const;
    // Touching a wall: -1 on the left, 1 on the right, 0 none.
    int WallSide(Entity e) const;

    // The nearest hit along a ray (dir needn't be normalised) within `max_distance`.
    // Solid tiles count as layer 1 (bit 0 of `mask`).
    bool Raycast(Vec2 origin, Vec2 dir, f32 max_distance, RayHit2D& out, u32 mask = 0xFFFFFFFFu) const;
    // Entities whose colliders overlap the box.
    std::vector<Entity> OverlapBox(Vec2 center, Vec2 half_extents, u32 mask = 0xFFFFFFFFu) const;

    usize BodyCount() const { return body_count_; }

private:
    World& world_;
    Resolvers2D tiles_;
    std::vector<Event2D> events_;
    std::vector<u64> grounded_, left_wall_, right_wall_; // entity keys
    std::vector<u64> pairs_;                             // last step's contacting pairs
    std::vector<std::pair<u64, Event2D>> last_info_;     // and what each was
    usize body_count_ = 0;
};

void RegisterPhysics2DComponents();

} // namespace aether::sprite2d

AETHER_REFLECT(aether::sprite2d::Vec2, 1, AETHER_FIELD(x, Field_EditAnywhere), AETHER_FIELD(y, Field_EditAnywhere))
AETHER_ENUM(aether::sprite2d::BodyType2D, 1, AETHER_ENUM_VALUE(Static), AETHER_ENUM_VALUE(Kinematic), AETHER_ENUM_VALUE(Dynamic))
AETHER_ENUM(aether::sprite2d::Shape2D, 1, AETHER_ENUM_VALUE(Box), AETHER_ENUM_VALUE(Circle))

AETHER_REFLECT(aether::sprite2d::Rigidbody2D, 1,
    AETHER_FIELD(type, Field_EditAnywhere),
    AETHER_FIELD(mass, Field_EditAnywhere, {.range_min = 0.001, .range_max = 100000.0, .units = "kg"}),
    AETHER_FIELD(gravity_scale, Field_EditAnywhere),
    AETHER_FIELD(linear_damping, Field_EditAnywhere, {.range_min = 0.0, .range_max = 100.0}),
    AETHER_FIELD(velocity, Field_EditAnywhere, {.units = "m/s"})
)

AETHER_REFLECT(aether::sprite2d::Collider2D, 1,
    AETHER_FIELD(shape, Field_EditAnywhere),
    AETHER_FIELD(half_extents, Field_EditAnywhere, {.tooltip = "The box's half sizes", .units = "m"}),
    AETHER_FIELD(radius, Field_EditAnywhere, {.tooltip = "The circle's radius", .range_min = 0.0, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(offset, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(friction, Field_EditAnywhere, {.range_min = 0.0, .range_max = 2.0}),
    AETHER_FIELD(restitution, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(trigger, Field_EditAnywhere, {.tooltip = "Reports overlaps, doesn't push"}),
    AETHER_FIELD(layer, Field_EditAnywhere),
    AETHER_FIELD(mask, Field_EditAnywhere, {.tooltip = "The layers this collides with"})
)
