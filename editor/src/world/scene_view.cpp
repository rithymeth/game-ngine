#include "world/scene_view.h"

#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "content/content_drag.h"
#include "core/snapping.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

namespace aether::editor {

namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kMaxPitch = 1.5f; // just short of straight up, where look-at has no horizontal to turn around

Vec3 WorldOrigin(const SceneDocument& document, Entity entity) {
    const Mat4 world = ComputeWorldTransform(document.GetWorld(), document.Guids(), entity);
    return Vec3(world.cols[3].x, world.cols[3].y, world.cols[3].z);
}

ImU32 Color(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

} // namespace

Vec3 SceneViewCamera::Eye() const {
    const f32 cp = std::cos(pitch);
    return target + Vec3(cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)) * distance;
}

Quaternion SceneViewCamera::Rotation() const {
    // Turn about Y by the yaw, then tip about the camera's own X: looking down (the eye above the target) is a negative turn about X.
    return (Quaternion::FromAxisAngle(Vec3(0, 1, 0), yaw) * Quaternion::FromAxisAngle(Vec3(1, 0, 0), -pitch)).Normalized();
}

Mat4 SceneViewCamera::View() const { return Mat4::LookAtRH(Eye(), target, Vec3(0.0f, 1.0f, 0.0f)); }

Mat4 SceneViewCamera::ViewProjection(f32 aspect) const {
    return Mat4::PerspectiveRH(fov_degrees * kPi / 180.0f, std::max(aspect, 0.01f), 0.1f, 2000.0f) * View();
}

void SceneViewCamera::Orbit(f32 delta_yaw, f32 delta_pitch) {
    yaw += delta_yaw;
    pitch = std::clamp(pitch + delta_pitch, -kMaxPitch, kMaxPitch);
}

void SceneViewCamera::Zoom(f32 wheel_steps) {
    distance = std::clamp(distance * std::pow(0.9f, wheel_steps), 0.5f, 1000.0f);
}

void SceneViewCamera::Pan(f32 right_units, f32 up_units) {
    // The camera's right and up in the world, from the view matrix's rows.
    const Mat4 view = View();
    const Vec3 right(view.cols[0].x, view.cols[1].x, view.cols[2].x);
    const Vec3 up(view.cols[0].y, view.cols[1].y, view.cols[2].y);
    target = target + right * right_units + up * up_units;
}

void HandleCameraInput(SceneViewCamera& camera, bool hovered, bool active, const ImVec2& size) {
    const ImGuiIO& io = ImGui::GetIO();
    if (active) {
        const ImVec2 d = io.MouseDelta;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            camera.Orbit(-d.x * 0.008f, d.y * 0.008f);
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (io.KeyShift && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
            const f32 per_pixel = 2.0f * camera.distance * std::tan(camera.fov_degrees * kPi / 360.0f) / std::max(size.y, 1.0f);
            camera.Pan(-d.x * per_pixel, d.y * per_pixel);
        }
    }
    if (hovered && io.MouseWheel != 0.0f) camera.Zoom(io.MouseWheel);
}

bool ProjectToViewport(const Mat4& view_projection, const Vec3& world, const ImVec2& origin, const ImVec2& size, ImVec2& out, f32* depth) {
    const Vec4 clip = view_projection * Vec4(world.x, world.y, world.z, 1.0f);
    if (!(clip.w > 1e-4f)) return false; // at or behind the eye
    const f32 ndc_x = clip.x / clip.w;
    const f32 ndc_y = clip.y / clip.w;
    out = ImVec2(origin.x + (ndc_x * 0.5f + 0.5f) * size.x, origin.y + (0.5f - ndc_y * 0.5f) * size.y);
    if (depth) *depth = clip.w;
    return true;
}

Entity PickEntity(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 radius) {
    if (size.x < 1.0f || size.y < 1.0f) return kNullEntity;
    const Mat4 vp = camera.ViewProjection(size.x / size.y);
    Entity best = kNullEntity;
    f32 best_distance = radius, best_depth = std::numeric_limits<f32>::max();
    for (const Entity entity : document.Entities()) {
        if (!document.GetWorld().HasComponent<Transform>(entity)) continue;
        ImVec2 at;
        f32 depth = 0.0f;
        if (!ProjectToViewport(vp, WorldOrigin(document, entity), origin, size, at, &depth)) continue;
        const f32 distance = std::hypot(at.x - mouse.x, at.y - mouse.y);
        if (distance > radius) continue;
        const bool nearer = distance < best_distance - 2.0f;
        const bool tie_but_closer_to_camera = std::fabs(distance - best_distance) <= 2.0f && depth < best_depth;
        if (best == kNullEntity || nearer || tie_but_closer_to_camera) {
            best = entity;
            best_distance = std::min(distance, best_distance);
            best_depth = depth;
        }
    }
    return best;
}

bool MouseRay(const Mat4& view_projection, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, Vec3& ray_origin, Vec3& ray_direction) {
    if (size.x < 1.0f || size.y < 1.0f) return false;
    Mat4 inverse;
    if (!view_projection.TryInverse(inverse)) return false;
    const f32 nx = (mouse.x - origin.x) / size.x * 2.0f - 1.0f;
    const f32 ny = 1.0f - (mouse.y - origin.y) / size.y * 2.0f;
    auto unproject = [&](f32 depth) {
        const Vec4 p = inverse * Vec4(nx, ny, depth, 1.0f);
        return Vec3(p.x / p.w, p.y / p.w, p.z / p.w);
    };
    const Vec3 near_point = unproject(0.0f), far_point = unproject(1.0f);
    const Vec3 d = far_point - near_point;
    if (!(d.Length() > 1e-6f)) return false;
    ray_origin = near_point;
    ray_direction = d.Normalized();
    return true;
}

bool GroundPoint(const Mat4& view_projection, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, Vec3& point) {
    Vec3 ray_origin, ray_direction;
    if (!MouseRay(view_projection, origin, size, mouse, ray_origin, ray_direction)) return false;
    if (std::fabs(ray_direction.y) < 1e-5f) return false;
    const f32 t = -ray_origin.y / ray_direction.y;
    if (t < 0.0f) return false;
    point = ray_origin + ray_direction * t;
    return true;
}

Entity PlaceModelAsset(SceneDocument& document, const std::string& content_path, const Vec3& position) {
    if (!IsPlaceableModel(content_path)) return kNullEntity;
    const std::string name = std::filesystem::path(content_path).stem().string();
    document.Commands().BeginTransaction("Place " + name);
    const Entity entity = document.CreateEntity(name.empty() ? "Model" : name, SceneEntityKind::Model, content_path);
    document.SetPosition(entity, position, true);
    document.Commands().EndTransaction();
    document.SetSelection({entity});
    return entity;
}

bool ClosestOnAxis(const Vec3& axis_origin, const Vec3& axis_direction, const Vec3& ray_origin, const Vec3& ray_direction, f32& distance) {
    const Vec3 w0 = axis_origin - ray_origin;
    const f32 b = axis_direction.Dot(ray_direction);
    const f32 d = axis_direction.Dot(w0);
    const f32 e = ray_direction.Dot(w0);
    const f32 denominator = 1.0f - b * b; // both directions are unit length
    if (std::fabs(denominator) < 1e-5f) return false;
    distance = (b * e - d) / denominator;
    return true;
}

namespace {

Vec3 AxisUnit(GizmoAxis axis) {
    return axis == GizmoAxis::X ? Vec3(1, 0, 0) : (axis == GizmoAxis::Y ? Vec3(0, 1, 0) : Vec3(0, 0, 1));
}

f32 AxisComponent(const Vec3& v, GizmoAxis axis) { return axis == GizmoAxis::X ? v.x : (axis == GizmoAxis::Y ? v.y : v.z); }

f32 DistanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    const f32 abx = b.x - a.x, aby = b.y - a.y;
    const f32 length_sq = abx * abx + aby * aby;
    const f32 t = length_sq > 1e-6f ? std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / length_sq, 0.0f, 1.0f) : 0.0f;
    return std::hypot(p.x - (a.x + abx * t), p.y - (a.y + aby * t));
}

// Two unit vectors that, with `axis`, make a right-handed frame: u x v = axis.
void PlaneBasis(const Vec3& axis, Vec3& u, Vec3& v) {
    const Vec3 candidate = std::fabs(axis.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    u = axis.Cross(candidate).Normalized();
    v = axis.Cross(u);
}

// The parent's world matrix inverse, or false for a root entity.
bool ParentInverse(const SceneDocument& document, Entity entity, Mat4& inverse) {
    const Entity parent = document.ParentOf(entity);
    if (parent == kNullEntity) return false;
    return ComputeWorldTransform(document.GetWorld(), document.Guids(), parent).TryInverse(inverse);
}

// A world position as the local position an entity under its parent would need to be there.
Vec3 LocalPositionFor(const SceneDocument& document, Entity entity, const Vec3& world_position) {
    Mat4 inverse;
    if (!ParentInverse(document, entity, inverse)) return world_position;
    const Vec4 local = inverse * Vec4(world_position.x, world_position.y, world_position.z, 1.0f);
    return Vec3(local.x, local.y, local.z);
}

// A world direction as the local direction an entity under its parent would turn about.
Vec3 LocalDirectionFor(const SceneDocument& document, Entity entity, const Vec3& world_direction) {
    Mat4 inverse;
    if (!ParentInverse(document, entity, inverse)) return world_direction;
    const Vec4 local = inverse * Vec4(world_direction.x, world_direction.y, world_direction.z, 0.0f);
    const Vec3 v(local.x, local.y, local.z);
    return v.Length() > 1e-6f ? v.Normalized() : world_direction;
}

} // namespace

std::vector<Entity> TransformGizmo::Subjects(const SceneDocument& document) {
    const World& world = document.GetWorld();
    auto movable = [&](Entity e) { return world.IsAlive(e) && world.HasComponent<Transform>(e); };
    std::vector<Entity> out;
    for (const Entity entity : document.Selection()) {
        if (!movable(entity) || std::find(out.begin(), out.end(), entity) != out.end()) continue;
        bool ancestor_selected = false; // walk up: a selected ancestor moves this one anyway
        Entity up = document.ParentOf(entity);
        for (int depth = 0; up != kNullEntity && depth < 256 && !ancestor_selected; ++depth, up = document.ParentOf(up)) {
            ancestor_selected = document.IsSelected(up);
        }
        if (!ancestor_selected) out.push_back(entity);
    }
    return out;
}

Entity TransformGizmo::Subject(const SceneDocument& document) {
    const std::vector<Entity> subjects = Subjects(document);
    return subjects.empty() ? kNullEntity : subjects[0];
}

Vec3 TransformGizmo::HandleDirection(const SceneDocument& document, Entity subject, GizmoAxis axis, GizmoMode mode) {
    if (mode != GizmoMode::Scale || subject == kNullEntity) return AxisUnit(axis);
    const Mat4 m = ComputeWorldTransform(document.GetWorld(), document.Guids(), subject);
    const Vec4& column = axis == GizmoAxis::X ? m.cols[0] : (axis == GizmoAxis::Y ? m.cols[1] : m.cols[2]);
    const Vec3 v(column.x, column.y, column.z);
    return v.Length() > 1e-6f ? v.Normalized() : AxisUnit(axis);
}

void TransformGizmo::RingPoints(const Vec3& centre, const Vec3& axis_direction, f32 radius, int count, Vec3* out) {
    Vec3 u, v;
    PlaneBasis(axis_direction, u, v);
    for (int i = 0; i < count; ++i) {
        const f32 a = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(count);
        out[i] = centre + u * (std::cos(a) * radius) + v * (std::sin(a) * radius);
    }
}

GizmoAxis TransformGizmo::HitTest(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, GizmoMode mode) const {
    const Entity subject = Subject(document);
    if (subject == kNullEntity || size.x < 1.0f || size.y < 1.0f) return GizmoAxis::None;
    const Mat4 vp = camera.ViewProjection(size.x / size.y);
    const Vec3 at = WorldOrigin(document, subject);
    ImVec2 from;
    if (!ProjectToViewport(vp, at, origin, size, from)) return GizmoAxis::None;
    GizmoAxis best = GizmoAxis::None;
    f32 best_distance = 9.0f; // pixels
    for (const GizmoAxis axis : {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z}) {
        f32 distance = 1e30f;
        if (mode == GizmoMode::Rotate) {
            constexpr int kSegments = 48;
            Vec3 ring[kSegments];
            RingPoints(at, AxisUnit(axis), HandleLength(camera), kSegments, ring);
            ImVec2 previous;
            bool have_previous = ProjectToViewport(vp, ring[kSegments - 1], origin, size, previous);
            for (int i = 0; i < kSegments; ++i) {
                ImVec2 current;
                const bool ok = ProjectToViewport(vp, ring[i], origin, size, current);
                if (ok && have_previous) distance = std::min(distance, DistanceToSegment(mouse, previous, current));
                previous = current;
                have_previous = ok;
            }
        } else {
            ImVec2 to;
            if (!ProjectToViewport(vp, at + HandleDirection(document, subject, axis, mode) * HandleLength(camera), origin, size, to)) continue;
            distance = DistanceToSegment(mouse, from, to);
        }
        if (distance < best_distance) { best_distance = distance; best = axis; }
    }
    return best;
}

bool TransformGizmo::Measure(const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32& value) const {
    Vec3 ray_origin, ray_direction;
    if (!MouseRay(camera.ViewProjection(size.x / size.y), origin, size, mouse, ray_origin, ray_direction)) return false;
    if (mode_ != GizmoMode::Rotate) return ClosestOnAxis(start_world_, direction_, ray_origin, ray_direction, value);
    // Rotate: where the ray meets the ring's plane, and the angle of that point around the axis.
    const f32 facing = ray_direction.Dot(direction_);
    if (std::fabs(facing) < 1e-4f) return false;
    const f32 t = (start_world_ - ray_origin).Dot(direction_) / facing;
    const Vec3 p = ray_origin + ray_direction * t - start_world_;
    Vec3 u, v;
    PlaneBasis(direction_, u, v);
    value = std::atan2(p.Dot(v), p.Dot(u));
    return true;
}

bool TransformGizmo::Begin(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, GizmoMode mode) {
    const GizmoAxis axis = HitTest(document, camera, origin, size, mouse, mode);
    if (axis == GizmoAxis::None) return false;
    const std::vector<Entity> entities = Subjects(document);
    if (entities.empty()) return false;
    // Set up the state the measurement needs, and take it back if the first measurement fails.
    const GizmoAxis old_axis = axis_;
    const GizmoMode old_mode = mode_;
    const Vec3 old_start = start_world_, old_direction = direction_;
    axis_ = axis;
    mode_ = mode;
    start_world_ = WorldOrigin(document, entities[0]);
    direction_ = HandleDirection(document, entities[0], axis, mode);
    f32 value = 0.0f;
    if (!Measure(camera, origin, size, mouse, value) || (mode != GizmoMode::Rotate && std::fabs(value) < 1e-3f)) {
        axis_ = old_axis;
        mode_ = old_mode;
        start_world_ = old_start;
        direction_ = old_direction;
        return false;
    }
    subjects_.clear();
    for (const Entity entity : entities) {
        const Transform* transform = document.GetWorld().GetComponent<Transform>(entity);
        if (!transform) continue;
        subjects_.push_back({entity, *transform, WorldOrigin(document, entity)});
    }
    grab_ = value;
    return !subjects_.empty();
}

bool TransformGizmo::Apply(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step) {
    if (!Dragging() || subjects_.empty()) return false;
    f32 value = 0.0f;
    if (!Measure(camera, origin, size, mouse, value)) return false;
    const GizmoAxis axis = axis_;
    const Held& first = subjects_[0];
    // What the drag amounts to, once, for the first subject (snapped); the others follow it.
    f32 along = 0.0f, ratio = 1.0f, angle = 0.0f;
    if (mode_ == GizmoMode::Move) {
        along = value - grab_;
        if (snap_step > 0.0f) along = SnapScalar(AxisComponent(start_world_, axis) + along, snap_step) - AxisComponent(start_world_, axis);
    } else if (mode_ == GizmoMode::Scale) {
        ratio = std::max(value / grab_, 0.01f); // the handle's own distance from the origin, relative to where it was taken
        const f32 start = AxisComponent(first.before.scale, axis);
        if (snap_step > 0.0f && std::fabs(start) > 1e-6f) ratio = SnapScaleAxis(start * ratio, snap_step) / start;
    } else {
        // The angle from where the mouse took hold, as the short way round. A rotation repeats every full turn, so
        // following the mouse through several would give the same result.
        angle = value - grab_;
        while (angle > kPi) angle -= 2.0f * kPi;
        while (angle < -kPi) angle += 2.0f * kPi;
        if (snap_step > 0.0f) angle = SnapRotationDegrees(angle * 180.0f / kPi, snap_step) * kPi / 180.0f;
    }
    const Vec3 k = direction_; // the axis a rotation turns about (unit length)
    document.Touch(); // the components are written below without a command, so tell a viewer the scene is moving
    for (const Held& held : subjects_) {
        if (!document.GetWorld().IsAlive(held.entity)) continue;
        Transform* transform = document.GetWorld().GetComponent<Transform>(held.entity);
        if (!transform) continue;
        if (mode_ == GizmoMode::Move) {
            transform->position = LocalPositionFor(document, held.entity, held.start_world + direction_ * along);
        } else if (mode_ == GizmoMode::Scale) {
            Vec3 scale = held.before.scale;
            (axis == GizmoAxis::X ? scale.x : (axis == GizmoAxis::Y ? scale.y : scale.z)) *= ratio;
            transform->scale = scale;
        } else {
            const Quaternion turn = Quaternion::FromAxisAngle(LocalDirectionFor(document, held.entity, direction_), angle);
            transform->rotation = (turn * held.before.rotation).Normalized();
            if (&held != &first) { // the others swing round the first one's position, as one rigid group
                const Vec3 v = held.start_world - start_world_;
                const f32 c = std::cos(angle), s = std::sin(angle);
                const Vec3 turned = v * c + k.Cross(v) * s + k * (k.Dot(v) * (1.0f - c));
                transform->position = LocalPositionFor(document, held.entity, start_world_ + turned);
            }
        }
    }
    return true;
}

bool TransformGizmo::Update(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step) {
    return Apply(document, camera, origin, size, mouse, snap_step);
}

void TransformGizmo::Commit(SceneDocument& document, const std::vector<Transform>& live) {
    const std::string what = mode_ == GizmoMode::Move ? "Move" : (mode_ == GizmoMode::Rotate ? "Rotate" : "Scale");
    document.Commands().BeginTransaction(subjects_.size() > 1 ? what + " " + std::to_string(subjects_.size()) + " entities" : what + " " + document.Name(subjects_[0].entity));
    for (usize i = 0; i < subjects_.size(); ++i) {
        const Held& held = subjects_[i];
        if (!document.GetWorld().IsAlive(held.entity)) continue;
        if (Transform* transform = document.GetWorld().GetComponent<Transform>(held.entity)) *transform = held.before; // as it was, so each edit is recorded from there
        // A command is recorded only for a field that differs from where it started.
        document.SetPosition(held.entity, live[i].position, true);
        document.SetRotation(held.entity, live[i].rotation, true);
        document.SetScale(held.entity, live[i].scale, true);
    }
    document.Commands().EndTransaction();
}

void TransformGizmo::End(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step) {
    if (Dragging() && !subjects_.empty()) {
        Apply(document, camera, origin, size, mouse, snap_step); // the final position of the mouse
        std::vector<Transform> live;
        for (const Held& held : subjects_) {
            const Transform* transform = document.GetWorld().IsAlive(held.entity) ? document.GetWorld().GetComponent<Transform>(held.entity) : nullptr;
            live.push_back(transform ? *transform : held.before);
        }
        Commit(document, live);
    }
    axis_ = GizmoAxis::None;
    subjects_.clear();
}

} // namespace aether::editor
