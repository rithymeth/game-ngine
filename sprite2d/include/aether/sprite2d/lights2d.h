#pragma once

#include "aether/core/base.h"
#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/components.h"
#include "aether/sprite2d/batch.h"
#include "aether/sprite2d/components.h"
#include "aether/sprite2d/physics2d.h"

#include <vector>

// 2D lights (Phase 26 step 5, docs/design/PHASE_SPECS.md §26.6): global
// ambient light, point lights and spot lights, with shadows cast by solid
// tilemap cells and ShadowCaster2D boxes. The module computes the lighting
// (the light at a point, a light map over a region, the visibility polygon
// of a light for drawing its shadows); a renderer uploads the light map or
// the polygons.

namespace aether::sprite2d {

enum class Light2DType : u8 { Global, Point, Spot };

struct Light2D {
    Light2DType type = Light2DType::Point;
    SpriteColor color;
    f32 intensity = 1.0f;
    f32 radius = 6.0f;           // world units; the light is 0 at this distance (point, spot)
    f32 falloff = 1.5f;          // 1 linear, higher drops faster
    f32 direction_degrees = 0.0f; // a spot's axis, 0 = +x, counter-clockwise (the entity's rotation adds)
    f32 inner_angle = 20.0f;     // a spot's full-bright half angle, degrees
    f32 outer_angle = 40.0f;     // and where it reaches 0
    bool cast_shadows = true;
    bool enabled = true;
    u32 layers = 0xFFFFFFFFu;    // the sorting layers it lights (a bit per layer); all by default
};

// A box that blocks light, for things that aren't tiles.
struct ShadowCaster2D {
    Vec2 half_extents{0.5f, 0.5f};
    Vec2 offset;
};

struct Segment2D {
    Vec2 a, b;
};

// An RGB sum. Values can exceed 1 (overlapping lights); the renderer clamps or tone-maps.
struct LightColor {
    f32 r = 0, g = 0, b = 0;
};

struct LightMap {
    Vec2 origin;       // the bottom-left corner of cell (0, 0)
    f32 cell_size = 1.0f;
    u32 width = 0, height = 0;
    std::vector<LightColor> cells; // width * height, row-major from the bottom row
    // Bilinear: smooth between cell centres; outside the map, the nearest edge cell.
    LightColor Sample(f32 x, f32 y) const;
    const LightColor& At(i32 x, i32 y) const;
};

// The edges that block light within `region` (min x, min y, max x, max y):
// each solid tile's edges that face an empty cell (so a wall is a line, not a
// grid), and every ShadowCaster2D box.
std::vector<Segment2D> GatherOccluders(const World& world, const Resolvers2D& tiles, Vec2 region_min, Vec2 region_max);

// Whether the straight line from `from` to `to` crosses any occluder.
bool Occluded(const std::vector<Segment2D>& occluders, Vec2 from, Vec2 to);

// The light each light gives `point`, summed with the global light.
// `occluders` (from GatherOccluders) lets shadow-casting lights be blocked.
LightColor LightAt(const World& world, const std::vector<Segment2D>& occluders, Vec2 point);

// The lighting over a rectangle of cells. Empty cells are lit by LightAt at
// their centres; a cell that is solid takes the brightest of its empty
// neighbours, so a wall's face glows like the air in front of it.
LightMap BuildLightMap(const World& world, const Resolvers2D& tiles, Vec2 region_min, Vec2 region_max, f32 cell_size);

// What a light can see: the polygon, counter-clockwise around `center`, of
// the points reached by rays that aren't blocked, within `radius`. For
// drawing the lit area (or, inverted, the shadow).
std::vector<Vec2> VisibilityPolygon(Vec2 center, f32 radius, const std::vector<Segment2D>& occluders);

void RegisterLight2DComponents();

} // namespace aether::sprite2d

AETHER_ENUM(aether::sprite2d::Light2DType, 1, AETHER_ENUM_VALUE(Global), AETHER_ENUM_VALUE(Point), AETHER_ENUM_VALUE(Spot))

AETHER_REFLECT(aether::sprite2d::Light2D, 1,
    AETHER_FIELD(type, Field_EditAnywhere),
    AETHER_FIELD(color, Field_EditAnywhere),
    AETHER_FIELD(intensity, Field_EditAnywhere, {.range_min = 0.0, .range_max = 16.0}),
    AETHER_FIELD(radius, Field_EditAnywhere, {.tooltip = "Where a point or spot light reaches 0", .range_min = 0.0, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(falloff, Field_EditAnywhere, {.tooltip = "1 is linear; higher drops faster", .range_min = 0.1, .range_max = 8.0}),
    AETHER_FIELD(direction_degrees, Field_EditAnywhere, {.tooltip = "A spot's axis; 0 is +x, counter-clockwise", .units = "deg"}),
    AETHER_FIELD(inner_angle, Field_EditAnywhere, {.tooltip = "A spot's full-bright half angle", .range_min = 0.0, .range_max = 180.0, .units = "deg"}),
    AETHER_FIELD(outer_angle, Field_EditAnywhere, {.tooltip = "Where a spot reaches 0", .range_min = 0.0, .range_max = 180.0, .units = "deg"}),
    AETHER_FIELD(cast_shadows, Field_EditAnywhere),
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(layers, Field_EditAnywhere)
)

AETHER_REFLECT(aether::sprite2d::ShadowCaster2D, 1,
    AETHER_FIELD(half_extents, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(offset, Field_EditAnywhere, {.units = "m"})
)
