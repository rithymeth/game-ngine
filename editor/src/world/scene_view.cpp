#include "world/scene_view.h"

#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cmath>
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

void SceneViewPanel::FocusSelection(const SceneDocument& document) {
    std::vector<Entity> subjects;
    for (const Entity entity : document.Selection()) {
        if (document.GetWorld().IsAlive(entity) && document.GetWorld().HasComponent<Transform>(entity)) subjects.push_back(entity);
    }
    if (subjects.empty()) {
        for (const Entity entity : document.Entities()) {
            if (document.GetWorld().HasComponent<Transform>(entity)) subjects.push_back(entity);
        }
    }
    if (subjects.empty()) return;
    Vec3 low(1e30f, 1e30f, 1e30f), high(-1e30f, -1e30f, -1e30f);
    for (const Entity entity : subjects) {
        const Vec3 p = WorldOrigin(document, entity);
        low = Vec3(std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z));
        high = Vec3(std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z));
    }
    camera_.target = (low + high) * 0.5f;
    const Vec3 extent = high - low;
    camera_.distance = std::clamp(std::max({extent.x, extent.y, extent.z}) * 1.6f + 5.0f, 3.0f, 1000.0f);
}

void SceneViewPanel::Draw(SceneDocument& document) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(avail.x, 64.0f), std::max(avail.y, 64.0f));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##scene_view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();

    // Input: right-drag orbits, middle-drag or shift + left-drag pans, the wheel zooms, F focuses, a click selects.
    if (active) {
        const ImVec2 d = io.MouseDelta;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            camera_.Orbit(-d.x * 0.008f, d.y * 0.008f);
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (io.KeyShift && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
            const f32 per_pixel = 2.0f * camera_.distance * std::tan(camera_.fov_degrees * kPi / 360.0f) / size.y;
            camera_.Pan(-d.x * per_pixel, d.y * per_pixel);
        }
    }
    if (hovered && io.MouseWheel != 0.0f) camera_.Zoom(io.MouseWheel);
    if (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F)) FocusSelection(document);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        press_ = io.MousePos;
        dragged_ = false;
    }
    if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && (std::fabs(io.MousePos.x - press_.x) + std::fabs(io.MousePos.y - press_.y)) > 4.0f) dragged_ = true;
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !dragged_ && !io.KeyShift) {
        const Entity picked = PickEntity(document, camera_, origin, size, io.MousePos);
        if (picked == kNullEntity) {
            if (!io.KeyCtrl) document.ClearSelection();
        } else if (io.KeyCtrl) {
            document.ToggleSelection(picked);
        } else {
            document.SetSelection({picked});
        }
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    draw->PushClipRect(origin, end, true);
    draw->AddRectFilled(origin, end, Color(22, 24, 29));
    const Mat4 vp = camera_.ViewProjection(size.x / size.y);
    auto line = [&](const Vec3& a, const Vec3& b, ImU32 color, f32 thickness = 1.0f) {
        ImVec2 pa, pb;
        if (ProjectToViewport(vp, a, origin, size, pa) && ProjectToViewport(vp, b, origin, size, pb)) draw->AddLine(pa, pb, color, thickness);
    };

    // The ground: a grid on y = 0, every fifth line stronger, with the world axes through the origin.
    constexpr int kHalf = 20;
    for (int i = -kHalf; i <= kHalf; ++i) {
        const ImU32 c = i % 5 == 0 ? Color(70, 76, 90) : Color(42, 46, 56);
        line(Vec3(static_cast<f32>(i), 0, static_cast<f32>(-kHalf)), Vec3(static_cast<f32>(i), 0, static_cast<f32>(kHalf)), c);
        line(Vec3(static_cast<f32>(-kHalf), 0, static_cast<f32>(i)), Vec3(static_cast<f32>(kHalf), 0, static_cast<f32>(i)), c);
    }
    line(Vec3(-kHalf, 0, 0), Vec3(kHalf, 0, 0), Color(150, 60, 60, 200), 1.5f);
    line(Vec3(0, 0, -kHalf), Vec3(0, 0, kHalf), Color(60, 90, 160, 200), 1.5f);
    line(Vec3(0, 0, 0), Vec3(0, 3, 0), Color(70, 150, 80, 220), 1.5f);

    // Each entity with a Transform: its unit box (the Transform's scale is the box's size), orange when selected, cyan for a camera.
    static const Vec3 corners[8] = {{-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f}, {.5f, .5f, -.5f}, {-.5f, .5f, -.5f},
                                    {-.5f, -.5f, .5f},  {.5f, -.5f, .5f},  {.5f, .5f, .5f},  {-.5f, .5f, .5f}};
    static const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    const World& world = document.GetWorld();
    Entity hovered_entity = kNullEntity;
    if (hovered) hovered_entity = PickEntity(document, camera_, origin, size, io.MousePos);
    for (const Entity entity : document.Entities()) {
        if (!world.HasComponent<Transform>(entity)) continue;
        const Mat4 m = ComputeWorldTransform(world, document.Guids(), entity);
        const bool selected = document.IsSelected(entity);
        const bool is_camera = world.HasComponent<::aether::Camera>(entity);
        const ImU32 color = selected ? Color(255, 170, 40) : (entity == hovered_entity ? Color(255, 255, 255) : (is_camera ? Color(80, 200, 220) : Color(150, 158, 175)));
        Vec3 w[8];
        for (int i = 0; i < 8; ++i) {
            const Vec4 p = m * Vec4(corners[i].x, corners[i].y, corners[i].z, 1.0f);
            w[i] = Vec3(p.x, p.y, p.z);
        }
        for (const auto& e : edges) line(w[e[0]], w[e[1]], color, selected ? 2.0f : 1.0f);
        const Vec3 origin_w(m.cols[3].x, m.cols[3].y, m.cols[3].z);
        if (selected) { // the axes of what is selected, in its own orientation
            const Vec3 x(m.cols[0].x, m.cols[0].y, m.cols[0].z), y(m.cols[1].x, m.cols[1].y, m.cols[1].z), z(m.cols[2].x, m.cols[2].y, m.cols[2].z);
            const f32 lx = std::max(x.Length(), 1e-4f), ly = std::max(y.Length(), 1e-4f), lz = std::max(z.Length(), 1e-4f);
            line(origin_w, origin_w + x * (1.2f / lx), Color(230, 70, 70), 2.0f);
            line(origin_w, origin_w + y * (1.2f / ly), Color(90, 210, 90), 2.0f);
            line(origin_w, origin_w + z * (1.2f / lz), Color(80, 120, 240), 2.0f);
        }
        if (selected || entity == hovered_entity) {
            ImVec2 at;
            if (ProjectToViewport(vp, origin_w, origin, size, at)) draw->AddText(ImVec2(at.x + 8.0f, at.y - 18.0f), color, document.Name(entity).c_str());
        }
    }
    draw->AddText(ImVec2(origin.x + 8.0f, end.y - 22.0f), Color(128, 135, 148),
                  "right-drag: look   middle-drag / shift+drag: pan   wheel: zoom   click: select   F: focus");
    draw->PopClipRect();
}

} // namespace aether::editor
