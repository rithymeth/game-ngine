#pragma once

#include "aether/math/math.h"
#include "aether/project/project.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/entity_guid.h"

#include <string>
#include <string_view>
#include <vector>

namespace aether {

// Gameplay framework components (Phase 9 step 5, docs/ROADMAP.md Phase 9):
// thin, ECS-friendly versions of Unreal/Unity concepts.

// ---------------------------------------------------------------------------
// Active: turning entities on and off
// ---------------------------------------------------------------------------

// An entity without this component is active. An inactive entity (and every
// entity under it) gets no update callbacks; Lifecycle fires OnDisable /
// OnEnable when that changes (use Lifecycle::SetActive during play).
struct Active {
    bool active = true;
};

// False if the entity or any ancestor has Active{false}.
bool IsActiveInHierarchy(const World& world, const GuidIndex& guids, Entity entity);

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

enum class Projection { Perspective, Orthographic };

// A camera looks down its entity's -Z axis (right-handed, Y up).
struct Camera {
    Projection projection = Projection::Perspective;
    f32 fov_degrees = 60.0f;   // vertical, perspective only
    f32 ortho_height = 10.0f;  // world units top to bottom, orthographic only
    f32 near_plane = 0.1f;
    f32 far_plane = 1000.0f;
    // The active camera is the highest-priority one on an active entity.
    i32 priority = 0;
};

// Right-handed orthographic projection with Vulkan's [0, 1] depth range
// (the counterpart of Mat4::PerspectiveRH).
Mat4 OrthographicRH(f32 width, f32 height, f32 z_near, f32 z_far);

Mat4 CameraProjection(const Camera& camera, f32 aspect);

// The inverse of the entity's world transform (a view matrix). Transforms
// are rotation + translation only, so this is exact.
Mat4 CameraView(const World& world, const GuidIndex& guids, Entity entity);

// The camera to render the game from: the highest priority among cameras on
// active entities; ties go to the earliest-created. kNullEntity if none.
Entity FindActiveCamera(const World& world, const GuidIndex& guids);

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

// Free-form labels for finding and filtering entities ("Enemy", "Pickup").
// Compared by exact, case-sensitive name.
struct Tags {
    std::vector<std::string> names;
};

bool HasTag(const World& world, Entity entity, std::string_view tag);
// Adds the Tags component if needed. False if the entity already had the tag.
bool AddTag(World& world, Entity entity, const std::string& tag);
bool RemoveTag(World& world, Entity entity, std::string_view tag);
// Every entity with the tag, in creation order.
std::vector<Entity> FindEntitiesWithTag(const World& world, std::string_view tag);

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

// Which of the project's named layers (ProjectSettings::layers, up to 32) an
// entity is on, for collision filtering and queries. Without this component
// an entity is on layer 0, "Default".
struct Layer {
    u8 index = 0;
};

using LayerMask = u32;
inline constexpr LayerMask kAllLayers = 0xffffffffu;
inline constexpr u32 kMaxLayers = 32;

// The layer's index by name, or -1.
i32 FindLayer(const ProjectSettings& settings, std::string_view name);
// A mask of the named layers; names that aren't layers go to `unknown`.
LayerMask MakeLayerMask(const ProjectSettings& settings, const std::vector<std::string>& names,
                        std::vector<std::string>* unknown = nullptr);
u8 LayerOf(const World& world, Entity entity);
bool IsInLayerMask(const World& world, Entity entity, LayerMask mask);

} // namespace aether

AETHER_REFLECT(aether::Active, 1, AETHER_FIELD(active, Field_EditAnywhere, {.tooltip = "Inactive entities and their children don't update"}))

AETHER_ENUM(aether::Projection, 1, AETHER_ENUM_VALUE(Perspective), AETHER_ENUM_VALUE(Orthographic))

AETHER_REFLECT(aether::Camera, 1,
    AETHER_FIELD(projection, Field_EditAnywhere),
    AETHER_FIELD(fov_degrees, Field_EditAnywhere, {.tooltip = "Vertical field of view", .range_min = 1, .range_max = 179, .units = "deg"}),
    AETHER_FIELD(ortho_height, Field_EditAnywhere, {.tooltip = "Orthographic view height", .range_min = 0.01, .range_max = 100000, .units = "m"}),
    AETHER_FIELD(near_plane, Field_EditAnywhere, {.range_min = 0.001, .range_max = 100000, .units = "m"}),
    AETHER_FIELD(far_plane, Field_EditAnywhere, {.range_min = 0.01, .range_max = 1000000, .units = "m"}),
    AETHER_FIELD(priority, Field_EditAnywhere, {.tooltip = "The highest-priority camera on an active entity is used"})
)

AETHER_REFLECT(aether::Tags, 1, AETHER_FIELD(names, Field_EditAnywhere, {.tooltip = "Labels for finding entities"}))

AETHER_REFLECT(aether::Layer, 1, AETHER_FIELD(index, Field_EditAnywhere, {.tooltip = "Index into the project's layers", .range_min = 0, .range_max = 31}))
