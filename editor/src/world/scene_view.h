#pragma once

#include "aether/ecs/entity.h"
#include "aether/math/mat4.h"
#include "aether/math/quaternion.h"
#include "core/scene_document.h"
#include "world/model_bounds.h"

#include <imgui.h>

#include <array>
#include <filesystem>
#include <optional>

#include <vector>

namespace aether::editor {

// How the Scene View projects: the perspective orbit camera, or an orthographic view (top, front, side, or any orientation).
enum class SceneViewProjection { Perspective, Orthographic };
// The axis-aligned orthographic views the Scene View offers; Free is any other orientation.
enum class SceneViewOrientation { Free, Top, Front, Side };

// An orbit camera around a target point, Y up: right-drag turns it, and moves it with W/A/S/D and Q/E (E up, Q down) at
// `fly_speed`; middle-drag (or shift + left-drag) pans and the wheel zooms (or, while flying, changes the speed). In an
// orthographic projection the same angles give a parallel view and the wheel changes `ortho_height` instead. Plain data
// and plain functions, so the maths is tested without a window.
struct SceneViewCamera {
    f32 yaw = 0.7f;      // radians around Y
    f32 pitch = 0.5f;    // radians above the horizon, kept clear of straight up and down
    f32 distance = 14.0f;
    Vec3 target{0.0f, 0.0f, 0.0f};
    f32 fov_degrees = 60.0f;
    SceneViewProjection projection = SceneViewProjection::Perspective;
    f32 ortho_height = 14.0f; // world units across the view's vertical when orthographic: its own zoom, kept apart from `distance`
    f32 fly_speed = 8.0f;     // metres per second the fly keys move the view, before the shift multiplier

    Vec3 Eye() const;
    // The world up the view uses: +Y, or -Z/+Z when looking straight down/up, where +Y would be along the view direction.
    Vec3 Up() const;
    // Target to eye in world units: `distance`, or in an orthographic view far enough back that the whole scene is in front.
    f32 EyeDistance() const;
    // The orientation a camera entity needs to look from Eye() at the target (a camera looks down its own -Z).
    Quaternion Rotation() const;
    Mat4 View() const;
    Mat4 ViewProjection(f32 aspect) const;
    // The world-space height the view covers at the target, for panning and for keeping the gizmo the same size on screen.
    f32 ViewHeight() const;
    void Orbit(f32 delta_yaw, f32 delta_pitch);
    void Zoom(f32 wheel_steps);          // each step scales the perspective distance, or the orthographic height, by 0.9 (in) or 1/0.9 (out)
    void Pan(f32 right_units, f32 up_units); // in the view plane, in world units
    // Translates eye and target together along the view's own forward, right and up, in world units (the fly keys).
    void Move(f32 forward_units, f32 right_units, f32 up_units);
    // An orthographic top, front or side view, with exactly the angles that imply; Free does nothing.
    void Look(SceneViewOrientation orientation);
    // The preset the camera is on -- orthographic with matching angles -- else Free.
    SceneViewOrientation Orientation() const;

    // Everything a bookmark puts back: where the view looks from and how it projects and zooms.
    struct State {
        f32 yaw = 0.7f;
        f32 pitch = 0.5f;
        f32 distance = 14.0f;
        Vec3 target{0.0f, 0.0f, 0.0f};
        f32 ortho_height = 14.0f;
        SceneViewProjection projection = SceneViewProjection::Perspective;
    };
    State GetState() const;
    void SetState(const State& state);
    static constexpr int kBookmarkCount = 9;
    bool SaveBookmark(int slot);    // 1..9; false for any other slot
    bool RecallBookmark(int slot);  // false for an empty slot (or any other)
    bool HasBookmark(int slot) const;

private:
    std::array<std::optional<State>, kBookmarkCount> bookmarks_;
};

// The mouse and keys of an orbit view, for the ImGui item just submitted (right-drag turns, middle-drag or shift + left-drag pans, the
// wheel zooms). `size` is the item's size in pixels. Shared by the Scene View and the live viewport.
void HandleCameraInput(SceneViewCamera& camera, bool hovered, bool active, const ImVec2& size);

// Where a world point lands in a viewport rectangle (top-left `origin`, `size` pixels); false when it is behind the camera.
bool ProjectToViewport(const Mat4& view_projection, const Vec3& world, const ImVec2& origin, const ImVec2& size, ImVec2& out, f32* depth = nullptr);

// An entity's box in its own space: the model's real extent if it has a ModelRenderer whose model `bounds` knows, else a unit box.
LocalBounds EntityBounds(const SceneDocument& document, Entity entity, const ModelBoundsCache* bounds);

// Where the mouse ray first meets an entity's box (the model's real bounds, placed by its world transform): the distance along the ray, false for a miss.
bool RayHitsEntityBox(const SceneDocument& document, Entity entity, const LocalBounds& box, const Vec3& ray_origin, const Vec3& ray_direction, f32& distance);

// Where the mouse ray first meets an entity's actual triangles (`triangles`, the model's, placed by the entity's world transform, either face of each): the distance along
// the ray, in the same units as RayHitsEntityBox's, false for a miss. So an L-shaped model is not hit in the empty corner of its box.
bool RayHitsEntityMesh(const SceneDocument& document, Entity entity, const ModelTriangles& triangles, const Vec3& ray_origin, const Vec3& ray_direction, f32& distance);

// The entity under `mouse`. With `bounds`, an entity whose model's box the mouse ray passes through is picked, the nearest along the ray first (so a big
// model is selected by clicking anywhere on it, and one in front hides one behind). Otherwise, and for entities with no model (a camera, a light, an empty),
// the one whose origin is nearest `mouse` within `radius` pixels (the nearer to the camera on a tie within 2 pixels). kNullEntity if none.
// Only entities with a Transform can be picked.
Entity PickEntity(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 radius = 14.0f,
                  const ModelBoundsCache* bounds = nullptr);

// Every entity under `mouse`, nearest first: those whose model box the ray passes through (with `bounds`), then those whose origin is within `radius`
// pixels, ordered by the distance along the ray. This is the stack Alt-click steps through, front to back.
std::vector<Entity> PickEntities(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse,
                                 f32 radius = 14.0f, const ModelBoundsCache* bounds = nullptr);

// The entities whose screen-space box meets the rectangle between `first` and `second` (either corner order): for marquee selection. A model's real box
// is used when `bounds` knows it, the unit box otherwise; an entity partly behind the near plane is judged by its origin.
std::vector<Entity> EntitiesInRectangle(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& first,
                                        const ImVec2& second, const ModelBoundsCache* bounds = nullptr);

// The mouse as a ray into the world (origin and unit direction); false if the view-projection can't be inverted.
bool MouseRay(const Mat4& view_projection, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, Vec3& ray_origin, Vec3& ray_direction);

// The distance along the line through `axis_origin` in unit direction `axis_direction` at which it passes closest to a ray;
// false when the two are (nearly) parallel, where there is no such point.
bool ClosestOnAxis(const Vec3& axis_origin, const Vec3& axis_direction, const Vec3& ray_origin, const Vec3& ray_direction, f32& distance);

// Where the mouse ray meets the ground plane (y = 0); false when it never does (parallel to the ground, or pointing away from it).
bool GroundPoint(const Mat4& view_projection, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, Vec3& point);

// The ground grid's minor spacing in metres: a power of ten, starting at one metre and growing by tens while a metre would draw the lines
// closer than ten pixels (so it is metres and decametres, then tens and hundreds, when the view is far out). Every tenth line is the major one.
f32 GridSpacing(f32 world_per_pixel);
// How opaque the grid is at `distance` from the view's centre: full to half `radius`, then easing to nothing at `radius`, so the ground
// has no edge to see.
f32 GridFade(f32 distance, f32 radius);

// Puts a model from the content folder into the scene as an entity named after the file with a ModelRenderer at `position`, selects it,
// and records the whole placement as one undoable step. Returns kNullEntity (and changes nothing) for a path that is not a glTF/GLB model.
// With `parent`, the entity is created under it (`position` is then relative to the parent: Vec3(0) puts it at the parent's origin).
Entity PlaceModelAsset(SceneDocument& document, const std::string& content_path, const Vec3& position, Entity parent = kNullEntity);

// Puts an instance of a prefab asset (`content_path`, a .aprefab under `content_root`) into the scene with its root at `position`, selects the root,
// and records the whole instance (the root and every entity under it) as one undoable step. The instance stays linked to the prefab, so later
// changes to the prefab reach it. Returns the root, or kNullEntity (and changes nothing, with the reason in `error`) for a path that is not a prefab
// asset of that content folder, or one that cannot be read or flattened. Reads the content folder's asset database (a scan, which writes .ameta files for assets that have none).
Entity PlacePrefabAsset(SceneDocument& document, const std::filesystem::path& content_root, const std::string& content_path, const Vec3& position, std::string* error = nullptr,
                        Entity parent = kNullEntity);

// X, Y and Z are the arrows; PlaneXY, PlaneXZ and PlaneYZ are the small squares (Move only) that slide the entity in that plane.
enum class GizmoAxis { None, X, Y, Z, PlaneXY, PlaneXZ, PlaneYZ };
enum class GizmoMode { Move, Rotate, Scale };
// Whose axes the Move arrows and Rotate rings follow: the world's, or the entity's own (Scale always uses the entity's own).
enum class GizmoSpace { World, Local };

// The transform gizmo on the first selected entity, in three modes. Move: arrows along world X, Y and Z. Rotate: a ring around
// each world axis. Scale: handles along the entity's own (local) axes. A drag is one undoable edit however many frames it takes
// (however many entities it moves), works in world space even under a parent, and snaps to `snap_step`, which means metres for Move,
// degrees for Rotate and a scale step for Scale (0 = off). With several entities selected they all follow: the handles sit on the first,
// Move shifts them all by the same amount, Scale applies the same ratio to each, and Rotate turns each about the same world axis and
// swings the others round the first's position. An entity whose ancestor is also selected is left to ride along with it. While the
// mouse is down the live values are written straight into the components; releasing puts the originals back and records the result
// inside one transaction, so it is one undo step. No ImGui: the panel feeds it the mouse.
class TransformGizmo {
public:
    // The handle length in world units, so it stays about the same size on screen at any zoom.
    static f32 HandleLength(const SceneViewCamera& camera) {
        return (camera.projection == SceneViewProjection::Orthographic ? camera.ortho_height : camera.distance) * 0.18f;
    }
    // The entity the gizmo is on (the first of Subjects), or kNullEntity.
    static Entity Subject(const SceneDocument& document);
    // The entities a drag moves: the selected, living ones with a Transform, leaving out any whose ancestor is also selected.
    static std::vector<Entity> Subjects(const SceneDocument& document);
    // The world-space direction of a handle: the world axis (Move, Rotate) or the entity's local axis (Scale).
    // With GizmoSpace::Local, Move and Rotate use the entity's local axis too.
    static Vec3 HandleDirection(const SceneDocument& document, Entity subject, GizmoAxis axis, GizmoMode mode, GizmoSpace space = GizmoSpace::World);
    static bool IsPlane(GizmoAxis axis) { return axis == GizmoAxis::PlaneXY || axis == GizmoAxis::PlaneXZ || axis == GizmoAxis::PlaneYZ; }
    // The two axes a plane handle spans (PlaneXZ: X and Z) and the one it is flat against (Y).
    static void PlaneAxes(GizmoAxis plane, GizmoAxis& first, GizmoAxis& second, GizmoAxis& normal);
    // The four world-space corners of a plane handle square on `subject`, in order round the square: the quarter to the half of the handle length along both axes.
    static void PlaneCorners(const SceneDocument& document, Entity subject, GizmoAxis plane, GizmoSpace space, f32 handle_length, Vec3* out);
    // Points of a rotate ring (a circle around `axis_direction`, radius `radius`, centred on `centre`), for drawing and hit-testing.
    static void RingPoints(const Vec3& centre, const Vec3& axis_direction, f32 radius, int count, Vec3* out);

    GizmoAxis HitTest(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, GizmoMode mode, GizmoSpace space = GizmoSpace::World) const;
    // Starts a drag if `mouse` is on a handle; true if it did.
    bool Begin(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, GizmoMode mode, GizmoSpace space = GizmoSpace::World);
    // Edits the subject to follow the mouse, as an uncommitted edit; false if there is no drag (or the mouse ray is parallel to what it needs).
    bool Update(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step);
    // Ends the drag, committing it as one edit.
    void End(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step);
    bool Dragging() const { return axis_ != GizmoAxis::None; }
    GizmoAxis Axis() const { return axis_; }
    GizmoMode Mode() const { return mode_; }
    GizmoSpace Space() const { return space_; }

private:
    // Writes the live values for `mouse` into every subject's Transform (no commands).
    bool Apply(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step);
    // Puts the originals back and records the live values as commands in one transaction.
    void Commit(SceneDocument& document, const std::vector<Transform>& live);
    // Move and Scale: where along the handle's line the mouse ray passes closest. Rotate: the angle of the mouse around the axis.
    bool Measure(const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32& value) const;
    // Plane handles: where the mouse ray meets the handle's plane through the drag's start.
    bool MeasurePlane(const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, Vec3& point) const;
    bool ApplyPlane(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step);

    struct Held {
        Entity entity = kNullEntity;
        Transform before;           // its local Transform when the drag began
        Vec3 start_world{0, 0, 0};  // its world position when the drag began
    };

    GizmoAxis axis_ = GizmoAxis::None;
    GizmoMode mode_ = GizmoMode::Move;
    GizmoSpace space_ = GizmoSpace::World;
    std::vector<Held> subjects_;            // the first is the one the handles are on
    Vec3 start_world_{0.0f, 0.0f, 0.0f};   // the first subject's world position when the drag began
    Vec3 direction_{1.0f, 0.0f, 0.0f};     // the handle's world direction (the axis a ring turns about)
    Vec3 plane_u_{1, 0, 0}, plane_v_{0, 1, 0}, grab_point_{0, 0, 0}; // a plane drag: the handle's two world directions, and where the mouse took hold
    f32 grab_ = 0.0f;                      // Move/Scale: where along the handle the mouse took hold; Rotate: the angle it took hold at
};

} // namespace aether::editor
