#include "world/scene_view_panel.h"
#include "world/content_drop.h"
#include "world/scene_panel.h"

#include "aether/reflection/registry.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "content/content_drag.h"
#include "core/snapping.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string_view>

namespace aether::editor {

namespace {

ImU32 Color(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// A reflected field of a component, or null when the type has no such field (the physics module is optional, so components are read by reflection).
template <typename T>
const T* Field(const reflect::TypeInfo& type, const void* component, const char* name) {
    const reflect::FieldInfo* field = type.FindField(name);
    return field != nullptr ? field->As<T>(component) : nullptr;
}

// Every collider the scene carries, as a wireframe: boxes, spheres and capsules, found by their reflected component names so the portable editor need not
// link the physics module. A trigger (a sensor) is drawn yellow, a solid collider green.
void DrawColliders(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size,
                   const std::function<void(const Vec3&, const Vec3&, ImU32, f32)>& line) {
    const World& world = document.GetWorld();
    auto place = [](const Mat4& matrix, const Vec3& point) {
        const Vec4 world_point = matrix * Vec4(point.x, point.y, point.z, 1.0f);
        return Vec3(world_point.x, world_point.y, world_point.z);
    };
    auto ring = [&](const Mat4& matrix, const Vec3& centre, const Vec3& axis_a, const Vec3& axis_b, f32 radius, ImU32 color) {
        constexpr int kSegments = 24;
        Vec3 previous = place(matrix, centre + axis_a * radius);
        for (int segment = 1; segment <= kSegments; ++segment) {
            const f32 angle = (static_cast<f32>(segment) / kSegments) * 2.0f * 3.14159265f;
            const Vec3 next = place(matrix, centre + axis_a * (std::cos(angle) * radius) + axis_b * (std::sin(angle) * radius));
            line(previous, next, color, 1.5f);
            previous = next;
        }
    };
    for (const Entity entity : document.Entities()) {
        if (!world.HasComponent<Transform>(entity) || document.IsHidden(entity)) continue;
        const Mat4 matrix = ComputeWorldTransform(world, document.Guids(), entity);
        for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
            if (!world.HasComponentRaw(entity, id)) continue;
            const ComponentInfo& info = GetComponentInfo(id);
            const void* component = world.GetComponentRaw(entity, id);
            if (info.reflected == nullptr || component == nullptr) continue;
            const reflect::TypeInfo& type = *info.reflected;
            const std::string_view name = type.name;
            if (name != "BoxCollider" && name != "SphereCollider" && name != "CapsuleCollider") continue;
            const bool* trigger = Field<bool>(type, component, "is_trigger");
            const ImU32 color = (trigger != nullptr && *trigger) ? Color(235, 200, 70, 210) : Color(110, 220, 120, 210);
            const Vec3* centre_field = Field<Vec3>(type, component, "center");
            const Vec3 centre = centre_field != nullptr ? *centre_field : Vec3(0.0f, 0.0f, 0.0f);
            if (name == "BoxCollider") {
                const Vec3* field = Field<Vec3>(type, component, "half_extents");
                const Vec3 half = field != nullptr ? *field : Vec3(0.5f, 0.5f, 0.5f);
                Vec3 corners[8];
                for (int corner = 0; corner < 8; ++corner) {
                    corners[corner] = place(matrix, centre + Vec3((corner & 1) ? half.x : -half.x, (corner & 2) ? half.y : -half.y, (corner & 4) ? half.z : -half.z));
                }
                static constexpr int kEdges[12][2] = {{0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3}, {2, 6}, {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7}};
                for (const auto& edge : kEdges) line(corners[edge[0]], corners[edge[1]], color, 1.5f);
            } else if (name == "SphereCollider") {
                const f32* field = Field<f32>(type, component, "radius");
                const f32 radius = field != nullptr ? *field : 0.5f;
                ring(matrix, centre, Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), radius, color);
                ring(matrix, centre, Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), radius, color);
                ring(matrix, centre, Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), radius, color);
            } else { // CapsuleCollider: upright (along Y), heights the whole capsule, caps included
                const f32* radius_field = Field<f32>(type, component, "radius");
                const f32* height_field = Field<f32>(type, component, "height");
                const f32 radius = radius_field != nullptr ? *radius_field : 0.35f;
                const f32 height = height_field != nullptr ? *height_field : 1.8f;
                const f32 half = std::max(height * 0.5f - radius, 0.0f);
                ring(matrix, centre + Vec3(0.0f, half, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), radius, color);
                ring(matrix, centre - Vec3(0.0f, half, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), radius, color);
                line(place(matrix, centre + Vec3(radius, half, 0.0f)), place(matrix, centre + Vec3(radius, -half, 0.0f)), color, 1.5f);
                line(place(matrix, centre + Vec3(-radius, half, 0.0f)), place(matrix, centre + Vec3(-radius, -half, 0.0f)), color, 1.5f);
                line(place(matrix, centre + Vec3(0.0f, half, radius)), place(matrix, centre + Vec3(0.0f, -half, radius)), color, 1.5f);
                line(place(matrix, centre + Vec3(0.0f, half, -radius)), place(matrix, centre + Vec3(0.0f, -half, -radius)), color, 1.5f);
            }
        }
    }
}

// A small icon at an entity's projected origin saying what it is, so the viewport does not read as a field of identical
// boxes: a camera's body and lens, a sun, a speaker, a page, sparks. Models keep the box alone (the box is the model).
void DrawKindIcon(ImDrawList* draw, const ImVec2& at, EntityKind kind, ImU32 color) {
    const f32 r = 7.0f;
    switch (kind) {
        case EntityKind::Camera:
            draw->AddRectFilled(ImVec2(at.x - r, at.y - r * 0.65f), ImVec2(at.x + r * 0.15f, at.y + r * 0.65f), IM_COL32(16, 17, 21, 210), 2.0f);
            draw->AddRect(ImVec2(at.x - r, at.y - r * 0.65f), ImVec2(at.x + r * 0.15f, at.y + r * 0.65f), color, 2.0f, 0, 1.5f);
            draw->AddCircle(ImVec2(at.x + r * 0.55f, at.y), r * 0.5f, color, 12, 1.5f);
            break;
        case EntityKind::Light:
            draw->AddCircleFilled(at, r * 0.45f, color, 12);
            for (int ray = 0; ray < 8; ++ray) {
                const f32 angle = static_cast<f32>(ray) * 3.14159265f / 4.0f;
                const ImVec2 dir(std::cos(angle), std::sin(angle));
                draw->AddLine(ImVec2(at.x + dir.x * r * 0.7f, at.y + dir.y * r * 0.7f),
                              ImVec2(at.x + dir.x * r * 1.3f, at.y + dir.y * r * 1.3f), color, 1.5f);
            }
            break;
        case EntityKind::Audio:
            draw->AddRectFilled(ImVec2(at.x - r, at.y - r * 0.35f), ImVec2(at.x - r * 0.4f, at.y + r * 0.35f), color);
            draw->AddTriangleFilled(ImVec2(at.x - r * 0.45f, at.y - r * 0.7f), ImVec2(at.x - r * 0.45f, at.y + r * 0.7f),
                                    ImVec2(at.x + r * 0.35f, at.y), color);
            for (int arc = 1; arc <= 2; ++arc) {
                draw->PathArcTo(ImVec2(at.x + r * 0.35f, at.y), r * 0.4f * static_cast<f32>(arc), -0.7f, 0.7f, 8);
                draw->PathStroke(color, 0, 1.5f);
            }
            break;
        case EntityKind::Script:
            draw->AddRect(ImVec2(at.x - r * 0.7f, at.y - r), ImVec2(at.x + r * 0.7f, at.y + r), color, 1.5f, 0, 1.5f);
            for (int line = -1; line <= 1; ++line)
                draw->AddLine(ImVec2(at.x - r * 0.45f, at.y + static_cast<f32>(line) * r * 0.45f),
                              ImVec2(at.x + r * (line == 1 ? 0.2f : 0.5f), at.y + static_cast<f32>(line) * r * 0.45f), color, 1.5f);
            break;
        case EntityKind::Particles:
            for (int spark = 0; spark < 4; ++spark) {
                const f32 angle = static_cast<f32>(spark) * 1.5707963f;
                draw->AddLine(ImVec2(at.x - std::cos(angle) * r, at.y - std::sin(angle) * r),
                              ImVec2(at.x + std::cos(angle) * r, at.y + std::sin(angle) * r), color, 1.5f);
            }
            draw->AddCircleFilled(at, 1.8f, color);
            break;
        case EntityKind::Physics:
            draw->AddCircle(at, r * 0.7f, color, 12, 1.5f);
            draw->AddRect(ImVec2(at.x - r * 0.3f, at.y + r * 0.3f), ImVec2(at.x + r * 0.6f, at.y + r * 0.9f), color, 1.0f, 0, 1.5f);
            break;
        case EntityKind::Volume:
            draw->AddRect(ImVec2(at.x - r, at.y - r * 0.7f), ImVec2(at.x + r, at.y + r * 0.7f), color, 1.5f, 0, 1.5f);
            draw->AddRect(ImVec2(at.x - r * 0.6f, at.y - r * 0.4f), ImVec2(at.x + r * 0.6f, at.y + r * 0.4f), color, 1.0f, 0, 1.0f);
            break;
        case EntityKind::Ui:
            draw->AddRect(ImVec2(at.x - r, at.y - r * 0.7f), ImVec2(at.x + r, at.y + r * 0.7f), color, 1.5f, 0, 1.5f);
            draw->AddRectFilled(ImVec2(at.x - r * 0.7f, at.y - r * 0.4f), ImVec2(at.x + r * 0.1f, at.y + r * 0.1f), color);
            break;
        case EntityKind::Empty:
        case EntityKind::Other:
            draw->AddCircle(at, r * 0.35f, color, 10, 1.5f);
            break;
        case EntityKind::Model: break; // the box says it
    }
}

// What a colour in the viewport means.
struct KindLegendEntry {
    EntityKind kind;
    const char* label;
};
constexpr KindLegendEntry kLegend[] = {
    {EntityKind::Camera, "Camera"}, {EntityKind::Light, "Light"},       {EntityKind::Model, "Model"},
    {EntityKind::Particles, "Particles"}, {EntityKind::Audio, "Audio"}, {EntityKind::Ui, "UI"},
    {EntityKind::Physics, "Physics"}, {EntityKind::Script, "Script"},   {EntityKind::Volume, "Volume"},
    {EntityKind::Other, "Entity"}, {EntityKind::Empty, "Empty"},
};

// The view modes the in-picture chip steps through, in the order it cycles them.
struct ViewModeEntry {
    SceneViewMode mode;
    const char* name;
};
constexpr ViewModeEntry kViewModes[] = {
    {SceneViewMode::Wireframe, "Wireframe"}, {SceneViewMode::Lit, "Lit"}, {SceneViewMode::Unlit, "Unlit"}, {SceneViewMode::Normals, "Normals"}};

// A control drawn into the picture rather than a widget above it, as the editor concept does: its rectangle, and whether
// the mouse is over it. The panel hit-tests these itself, so a click on one never reaches selection or the gizmo.
struct PictureChip {
    ImVec2 min{};
    ImVec2 max{};
    bool hovered = false;
};

constexpr f32 kChipHeight = 22.0f;
constexpr f32 kChipPadding = 9.0f;
constexpr f32 kChipGap = 6.0f;
constexpr f32 kChipCaret = 14.0f; // room for the little triangle of a chip that stands for a choice

PictureChip PlaceChip(f32& x, f32 y, const char* text, const ImVec2& mouse, bool in_view, bool caret = false) {
    PictureChip chip;
    chip.min = ImVec2(x, y);
    chip.max = ImVec2(x + ImGui::CalcTextSize(text).x + kChipPadding * 2.0f + (caret ? kChipCaret : 0.0f), y + kChipHeight);
    chip.hovered = in_view && mouse.x >= chip.min.x && mouse.x <= chip.max.x && mouse.y >= chip.min.y && mouse.y <= chip.max.y;
    x = chip.max.x + kChipGap;
    return chip;
}

PictureChip PlaceChipRight(f32& right, f32 y, const char* text, const ImVec2& mouse, bool in_view) {
    const f32 width = ImGui::CalcTextSize(text).x + kChipPadding * 2.0f;
    PictureChip chip;
    chip.max = ImVec2(right, y + kChipHeight);
    chip.min = ImVec2(right - width, y);
    chip.hovered = in_view && mouse.x >= chip.min.x && mouse.x <= chip.max.x && mouse.y >= chip.min.y && mouse.y <= chip.max.y;
    right = chip.min.x - kChipGap;
    return chip;
}

void DrawChip(ImDrawList* draw, const PictureChip& chip, const char* text, bool interactive, bool caret = false) {
    const bool lit = interactive && chip.hovered;
    draw->AddRectFilled(chip.min, chip.max, lit ? Color(31, 35, 44, 235) : Color(15, 17, 22, 215), 4.0f);
    draw->AddRect(chip.min, chip.max, lit ? Color(115, 125, 145, 235) : Color(58, 64, 76, 220), 4.0f);
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const f32 text_x = chip.min.x + kChipPadding;
    draw->AddText(ImVec2(text_x, chip.min.y + (kChipHeight - text_size.y) * 0.5f), lit ? Color(242, 245, 250) : Color(204, 210, 220), text);
    if (caret) {
        const ImVec2 centre(chip.max.x - kChipPadding - 2.0f, chip.min.y + kChipHeight * 0.5f);
        draw->AddTriangleFilled(ImVec2(centre.x - 3.5f, centre.y - 2.0f), ImVec2(centre.x + 3.5f, centre.y - 2.0f), ImVec2(centre.x, centre.y + 2.5f),
                                Color(165, 172, 185));
    }
}

// A camera's frustum, a point light's range, a spot light's cone and a directional light's direction, drawn where they
// are: the picture shows what an entity does, not just that a box is there. The lens, range and angles are the entity's
// own -- the Camera component, or the light's reflected fields -- so the portable editor need not link the renderer.
void DrawKindGizmo(const SceneDocument& document, Entity entity, EntityKind kind,
                   const std::function<void(const Vec3&, const Vec3&, ImU32, f32)>& line, ImU32 color) {
    const World& world = document.GetWorld();
    const Mat4 matrix = ComputeWorldTransform(world, document.Guids(), entity);
    const Vec4& position = matrix.cols[3];
    const Vec3 origin(position.x, position.y, position.z);
    auto axis = [&](int column) {
        const Vec4& column_data = matrix.cols[column];
        return Vec3(column_data.x, column_data.y, column_data.z).Normalized();
    };
    auto ring = [&](const Vec3& centre, const Vec3& a, const Vec3& b, f32 radius) {
        constexpr int kSegments = 24;
        Vec3 previous = centre + a * radius;
        for (int segment = 1; segment <= kSegments; ++segment) {
            const f32 angle = static_cast<f32>(segment) / kSegments * 2.0f * 3.14159265f;
            const Vec3 next = centre + a * (radius * std::cos(angle)) + b * (radius * std::sin(angle));
            line(previous, next, color, 1.2f);
            previous = next;
        }
    };
    if (kind == EntityKind::Camera) {
        const Camera* camera = world.GetComponent<Camera>(entity);
        const f32 fov = camera != nullptr ? std::clamp(camera->fov_degrees, 5.0f, 170.0f) : 60.0f;
        const bool orthographic = camera != nullptr && camera->projection == Projection::Orthographic;
        const Vec3 right = axis(0), up = axis(1);
        const Vec3 forward = axis(2) * -1.0f; // a camera looks down its own -Z
        const f32 depth = 2.5f;
        const f32 half_height = orthographic ? 0.9f : depth * std::tan(fov * 0.5f * 3.14159265f / 180.0f);
        const f32 half_width = half_height * 1.6f;
        const Vec3 centre = origin + forward * depth;
        const Vec3 corners[4] = {centre + right * half_width + up * half_height, centre - right * half_width + up * half_height,
                                 centre - right * half_width - up * half_height, centre + right * half_width - up * half_height};
        for (const Vec3& corner : corners) line(origin, corner, color, 1.2f);
        for (int corner = 0; corner < 4; ++corner) line(corners[corner], corners[(corner + 1) % 4], color, 1.2f);
        line(origin, origin + up * 0.7f, color, 2.0f); // which way is up in its picture
        return;
    }
    if (kind != EntityKind::Light) return;
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        if (!world.HasComponentRaw(entity, id)) continue;
        const ComponentInfo& info = GetComponentInfo(id);
        if (info.reflected == nullptr) continue;
        const void* component = world.GetComponentRaw(entity, id);
        if (component == nullptr) continue;
        std::string_view name = info.name;
        if (const usize colons = name.rfind("::"); colons != std::string_view::npos) name.remove_prefix(colons + 2);
        if (name == "PointLight") {
            const f32* range = Field<f32>(*info.reflected, component, "range");
            const f32 radius = std::clamp(range != nullptr ? *range : 10.0f, 0.1f, 500.0f);
            ring(origin, Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), radius);
            ring(origin, Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), radius);
            ring(origin, Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), radius);
        } else if (name == "SpotLight") {
            const f32* range = Field<f32>(*info.reflected, component, "range");
            const f32* outer = Field<f32>(*info.reflected, component, "outer_angle_degrees");
            const f32 reach = std::clamp(range != nullptr ? *range : 10.0f, 0.1f, 500.0f);
            const f32 half_angle = std::clamp(outer != nullptr ? *outer : 30.0f, 1.0f, 89.0f) * 3.14159265f / 180.0f;
            const Vec3 forward = axis(2) * -1.0f, right = axis(0), up = axis(1);
            const f32 radius = reach * std::tan(half_angle);
            const Vec3 disc = origin + forward * reach;
            ring(disc, right, up, radius);
            for (int spoke = 0; spoke < 4; ++spoke) {
                const f32 angle = static_cast<f32>(spoke) * 1.5707963f;
                line(origin, disc + right * (radius * std::cos(angle)) + up * (radius * std::sin(angle)), color, 1.2f);
            }
        } else if (name == "DirectionalLight") {
            const Vec3 forward = axis(2) * -1.0f; // the way its light travels
            const Vec3 tip = origin + forward * 2.5f;
            const Vec3 right = axis(0), up = axis(1);
            line(origin, tip, color, 2.0f);
            line(tip, tip - forward * 0.6f + right * 0.35f, color, 2.0f);
            line(tip, tip - forward * 0.6f - right * 0.35f, color, 2.0f);
            line(tip, tip - forward * 0.6f + up * 0.35f, color, 2.0f);
            line(tip, tip - forward * 0.6f - up * 0.35f, color, 2.0f);
            ring(origin, right, up, 0.5f); // the sun disc, facing the way it shines
        }
    }
}

} // namespace

void SceneViewPanel::FocusSelection(const SceneDocument& document, const ModelBoundsCache* bounds) {
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
        // The entity's real box, placed by its world transform, so a tall or off-centre model is framed as a whole rather than by its origin.
        const Mat4 world = ComputeWorldTransform(document.GetWorld(), document.Guids(), entity);
        const LocalBounds box = EntityBounds(document, entity, bounds);
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 local((corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y, (corner & 4) ? box.max.z : box.min.z);
            const Vec4 placed = world * Vec4(local.x, local.y, local.z, 1.0f);
            const Vec3 p(placed.x, placed.y, placed.z);
            low = Vec3(std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z));
            high = Vec3(std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z));
        }
    }
    camera_.target = (low + high) * 0.5f;
    const Vec3 extent = high - low;
    const f32 span = std::max({extent.x, extent.y, extent.z});
    camera_.distance = std::clamp(span * 1.6f + 5.0f, 3.0f, 20000.0f);
    camera_.ortho_height = std::clamp(span * 1.6f + 5.0f, 3.0f, 100000.0f); // the orthographic zoom is its own, so focus sets both
}

void SceneViewPanel::Draw(SceneDocument& document, const ModelBoundsCache* bounds, const std::filesystem::path& content_root) {
    tools_.SetBounds(bounds);
    tools_.DrawToolbar();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    // Perspective or an orthographic top/front/side view; choosing a preset switches to orthographic, and each projection keeps its own zoom.
    const bool ortho = camera_.projection == SceneViewProjection::Orthographic;
    const SceneViewOrientation orientation = camera_.Orientation();
    auto view_button = [](const char* label, bool active) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool pressed = ImGui::Button(label);
        if (active) ImGui::PopStyleColor();
        return pressed;
    };
    if (view_button("Persp", !ortho)) camera_.projection = SceneViewProjection::Perspective;
    ImGui::SetItemTooltip("Perspective orbit camera");
    ImGui::SameLine();
    if (view_button("Ortho", ortho)) camera_.projection = SceneViewProjection::Orthographic;
    ImGui::SetItemTooltip("Orthographic projection, keeping the current angle");
    ImGui::SameLine();
    if (view_button("Top", orientation == SceneViewOrientation::Top)) camera_.Look(SceneViewOrientation::Top);
    ImGui::SetItemTooltip("Top view: orthographic, looking down -Y");
    ImGui::SameLine();
    if (view_button("Front", orientation == SceneViewOrientation::Front)) camera_.Look(SceneViewOrientation::Front);
    ImGui::SetItemTooltip("Front view: orthographic, looking along -Z");
    ImGui::SameLine();
    if (view_button("Side", orientation == SceneViewOrientation::Side)) camera_.Look(SceneViewOrientation::Side);
    ImGui::SetItemTooltip("Side view: orthographic, looking along -X");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    // Wireframe is the boxes below; the rest are the real renderer (the shell's shading), Lit being its ordinary look.
    ImGui::SetNextItemWidth(112.0f);
    int mode_index = static_cast<int>(mode);
    constexpr const char* kModes[] = {"Wireframe", "Lit", "Unlit", "Normals"};
    if (ImGui::Combo("##view_mode", &mode_index, kModes, IM_ARRAYSIZE(kModes))) mode = static_cast<SceneViewMode>(mode_index);
    ImGui::SetItemTooltip("Wireframe: boxes over the grid. Lit, Unlit and Normals: the scene drawn by the renderer (needs the Vulkan editor)");
    ImGui::SameLine();
    if (ImGui::Button("Show")) ImGui::OpenPopup("##show_flags");
    ImGui::SetItemTooltip("Which overlays are drawn");
    if (ImGui::BeginPopup("##show_flags")) {
        ImGui::Checkbox("Grid", &show.grid);
        ImGui::Checkbox("Gizmos", &show.gizmos);
        ImGui::Checkbox("Bounds", &show.bounds);
        ImGui::Checkbox("Helpers", &show.helpers);
        ImGui::Checkbox("Collision", &show.collision);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", drop_.note.empty() ? "drop a model or a prefab from the Content Browser to place it" : drop_.note.c_str());
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(avail.x, 64.0f), std::max(avail.y, 64.0f));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    ImGui::InvisibleButton("##scene_view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    // An asset dragged in from the Content Browser: a model or a prefab is placed on the ground under the mouse (rounded to the move step if Snap is on).
    drop_.Accept(document, content_root, camera_, origin, size, tools_.MoveSnapStep());
    const ImGuiIO& io = ImGui::GetIO();

    // The picture's own controls, where the editor concept puts them: the viewport's name and look along the top left, the
    // gizmo's space, snapping and the grid along the top right, the selection at the bottom left and the scene's counts at
    // the bottom right. They are drawn into the picture and hit-tested here, so a click on one never selects or moves what
    // is under it.
    const bool ortho_now = camera_.projection == SceneViewProjection::Orthographic;
    const char* projection_text = ortho_now ? "Orthographic" : "Perspective";
    auto snap_label = [&] {
        if (tools_.SnapStep() <= 0.0f) return std::string("Snap off");
        char text[32];
        if (tools_.Mode() == GizmoMode::Move) std::snprintf(text, sizeof text, "Snap %.2g m", static_cast<f64>(tools_.SnapStep()));
        else if (tools_.Mode() == GizmoMode::Rotate) std::snprintf(text, sizeof text, "Snap %.0f deg", static_cast<f64>(tools_.SnapStep()));
        else std::snprintf(text, sizeof text, "Snap %.2g", static_cast<f64>(tools_.SnapStep()));
        return std::string(text);
    };
    const std::string snap_before = snap_label();
    f32 chip_x = origin.x + 10.0f;
    const f32 chip_y = origin.y + 10.0f;
    const PictureChip viewport_label = PlaceChip(chip_x, chip_y, "Viewport 1", io.MousePos, false);
    const PictureChip projection_chip = PlaceChip(chip_x, chip_y, projection_text, io.MousePos, hovered, true);
    const PictureChip view_mode_chip = PlaceChip(chip_x, chip_y, kViewModes[static_cast<int>(mode)].name, io.MousePos, hovered, true);
    const PictureChip show_chip = PlaceChip(chip_x, chip_y, "Show", io.MousePos, hovered, true);
    f32 chip_right = end.x - 10.0f;
    const PictureChip grid_chip = PlaceChipRight(chip_right, chip_y, show.grid ? "Grid on" : "Grid off", io.MousePos, hovered);
    const PictureChip snap_chip = PlaceChipRight(chip_right, chip_y, snap_before.c_str(), io.MousePos, hovered);
    const PictureChip space_chip = PlaceChipRight(chip_right, chip_y, tools_.Space() == GizmoSpace::Local ? "Local" : "World", io.MousePos, hovered);
    const std::vector<Entity> selection = document.Selection();
    const std::string selection_text = selection.empty() ? "Nothing selected"
                                      : selection.size() == 1 ? document.Name(selection.front()) + "  |  Selected"
                                                              : std::to_string(selection.size()) + " selected";
    char stats_text[96];
    std::snprintf(stats_text, sizeof stats_text, "%zu entities  |  %zu selected  |  %d fps", document.Entities().size(), selection.size(),
                  static_cast<int>(std::lround(io.Framerate)));
    f32 bottom_left = origin.x + 10.0f;
    const f32 bottom_y = end.y - kChipHeight - 10.0f;
    const PictureChip selection_chip = PlaceChip(bottom_left, bottom_y, selection_text.c_str(), io.MousePos, false);
    f32 bottom_right = end.x - 10.0f;
    const PictureChip stats_chip = PlaceChipRight(bottom_right, bottom_y, stats_text, io.MousePos, false);

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (projection_chip.hovered) camera_.projection = ortho_now ? SceneViewProjection::Perspective : SceneViewProjection::Orthographic;
        if (view_mode_chip.hovered) mode = kViewModes[(static_cast<int>(mode) + 1) % IM_ARRAYSIZE(kViewModes)].mode;
        if (show_chip.hovered) ImGui::OpenPopup("##scene_view_show_flags");
        if (space_chip.hovered) tools_.SetSpace(tools_.Space() == GizmoSpace::Local ? GizmoSpace::World : GizmoSpace::Local);
        if (snap_chip.hovered) tools_.SetSnap(!tools_.SnapEnabled());
        if (grid_chip.hovered) show.grid = !show.grid;
    }
    const bool over_chrome =
        projection_chip.hovered || view_mode_chip.hovered || show_chip.hovered || space_chip.hovered || snap_chip.hovered || grid_chip.hovered;
    const char* drawn_projection = camera_.projection == SceneViewProjection::Orthographic ? "Orthographic" : "Perspective";
    const char* drawn_mode = kViewModes[static_cast<int>(mode)].name;
    chrome_ = Chrome{};
    chrome_.viewport_label_min = viewport_label.min;
    chrome_.viewport_label_max = viewport_label.max;
    chrome_.projection_min = projection_chip.min;
    chrome_.projection_max = projection_chip.max;
    chrome_.view_mode_min = view_mode_chip.min;
    chrome_.view_mode_max = view_mode_chip.max;
    chrome_.show_min = show_chip.min;
    chrome_.show_max = show_chip.max;
    chrome_.space_min = space_chip.min;
    chrome_.space_max = space_chip.max;
    chrome_.snap_min = snap_chip.min;
    chrome_.snap_max = snap_chip.max;
    chrome_.grid_min = grid_chip.min;
    chrome_.grid_max = grid_chip.max;
    chrome_.selection_min = selection_chip.min;
    chrome_.selection_max = selection_chip.max;
    chrome_.stats_min = stats_chip.min;
    chrome_.stats_max = stats_chip.max;

    // Input: right-drag looks and W/A/S/D (Q/E down/up) fly, middle-drag or shift + left-drag pans, the wheel zooms (changes the speed while
    // flying), F focuses, number keys are camera bookmarks; the tools take the gizmo and the clicks.
    HandleCameraInput(camera_, hovered, active, size);
    if (hovered && !io.WantTextInput) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right) && io.MouseWheel != 0.0f) speed_notice_ = 1.5f;
        if (ImGui::IsKeyPressed(ImGuiKey_F)) FocusSelection(document, bounds);
        constexpr ImGuiKey kSlotKeys[SceneViewCamera::kBookmarkCount] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5,
                                                                        ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9};
        if (!io.KeyAlt) {
            for (int slot = 1; slot <= SceneViewCamera::kBookmarkCount; ++slot) {
                if (!ImGui::IsKeyPressed(kSlotKeys[slot - 1])) continue;
                if (io.KeyCtrl) {
                    if (camera_.SaveBookmark(slot)) {
                        notice_ = "Bookmark " + std::to_string(slot) + " saved";
                        notice_left_ = 1.5f;
                    }
                } else if (camera_.RecallBookmark(slot)) {
                    notice_ = "Bookmark " + std::to_string(slot);
                    notice_left_ = 1.5f;
                }
            }
        }
    }
    notice_left_ = std::max(0.0f, notice_left_ - io.DeltaTime);
    speed_notice_ = std::max(0.0f, speed_notice_ - io.DeltaTime);
    tools_.HandleInput(document, camera_, origin, size, hovered && !over_chrome, active && !over_chrome);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, end, true);
    draw->AddRectFilled(origin, end, Color(22, 24, 29));
    // A rendered picture behind the overlays: the shell's shading draws the scene with the real renderer at this view's camera and size. Without one
    // (a portable or headless editor) the boxes and grid above are the view, and it says so.
    const bool shaded = mode != SceneViewMode::Wireframe && shading != nullptr;
    if (shaded) {
        shading->Advance(document, camera_, static_cast<u32>(std::max(size.x, 1.0f)), static_cast<u32>(std::max(size.y, 1.0f)), mode, io.DeltaTime);
        if (shading->Picture().Valid()) {
            draw->AddImage(shading->Picture().Ref(), origin, end);
        } else if (!shading->Note().empty()) {
            const ImVec2 extent = ImGui::CalcTextSize(shading->Note().c_str());
            draw->AddText(ImVec2(origin.x + (size.x - extent.x) * 0.5f, origin.y + (size.y - extent.y) * 0.5f), Color(235, 238, 245, 220), shading->Note().c_str());
        }
    } else if (shading != nullptr) {
        shading->Stop(); // off screen: the running scene is let go until a shaded look is asked for again
    } else if (mode != SceneViewMode::Wireframe) {
        const char* text = "The Lit, Unlit and Normals views need the Vulkan editor: this build draws Wireframe only.";
        const ImVec2 extent = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2(origin.x + (size.x - extent.x) * 0.5f, origin.y + (size.y - extent.y) * 0.5f), Color(235, 238, 245, 220), text);
    }
    const Mat4 vp = camera_.ViewProjection(size.x / size.y);
    auto line = [&](const Vec3& a, const Vec3& b, ImU32 color, f32 thickness = 1.0f) {
        ImVec2 pa, pb;
        if (ProjectToViewport(vp, a, origin, size, pa) && ProjectToViewport(vp, b, origin, size, pb)) draw->AddLine(pa, pb, color, thickness);
    };

    // The ground: an endless grid on y = 0, fading out towards its edge so there is no edge to see. Its spacing is a power of ten picked to
    // keep the minor lines apart on screen (metres and decametres, then tens and hundreds further out), every tenth line stronger; drawn in
    // pieces so each line fades along its length as well. The world axes through the origin are coloured, with X and Z marked near it.
    if (show.grid) {
        const f32 world_per_pixel = camera_.ViewHeight() / size.y;
        const f32 step = GridSpacing(world_per_pixel);
        const f32 radius = std::max(camera_.ViewHeight() * 3.0f, step * 20.0f);
        const f32 major = step * 10.0f;
        const Vec3 centre = camera_.target;
        constexpr int kChunks = 6;
        auto ground_line = [&](const Vec3& a, const Vec3& b, ImU32 base) {
            for (int chunk = 0; chunk < kChunks; ++chunk) {
                const Vec3 p0 = a + (b - a) * (static_cast<f32>(chunk) / kChunks);
                const Vec3 p1 = a + (b - a) * (static_cast<f32>(chunk + 1) / kChunks);
                const Vec3 mid = (p0 + p1) * 0.5f;
                const f32 fade = GridFade(std::hypot(mid.x - centre.x, mid.z - centre.z), radius);
                if (!(fade > 0.002f)) continue;
                const ImU32 color = (base & 0x00FFFFFFu) | (static_cast<ImU32>(static_cast<f32>(base >> 24) * fade) << 24);
                line(p0, p1, color);
            }
        };
        const f32 first_x = std::floor((centre.x - radius) / step) * step, first_z = std::floor((centre.z - radius) / step) * step;
        for (f32 x = first_x; x <= centre.x + radius; x += step) {
            const bool strong = std::fabs(std::remainder(x, major)) < step * 0.5f;
            ground_line(Vec3(x, 0.0f, centre.z - radius), Vec3(x, 0.0f, centre.z + radius), strong ? Color(70, 76, 90) : Color(42, 46, 56));
        }
        for (f32 z = first_z; z <= centre.z + radius; z += step) {
            const bool strong = std::fabs(std::remainder(z, major)) < step * 0.5f;
            ground_line(Vec3(centre.x - radius, 0.0f, z), Vec3(centre.x + radius, 0.0f, z), strong ? Color(70, 76, 90) : Color(42, 46, 56));
        }
        ground_line(Vec3(centre.x - radius, 0.0f, 0.0f), Vec3(centre.x + radius, 0.0f, 0.0f), Color(150, 60, 60, 220));
        ground_line(Vec3(0.0f, 0.0f, centre.z - radius), Vec3(0.0f, 0.0f, centre.z + radius), Color(60, 90, 160, 220));
        line(Vec3(0, 0, 0), Vec3(0, 3, 0), Color(70, 150, 80, 220), 1.5f);
        const f32 label_distance = major * 2.0f;
        ImVec2 label_at;
        if (ProjectToViewport(vp, Vec3(label_distance, 0.0f, 0.0f), origin, size, label_at)) draw->AddText(ImVec2(label_at.x + 3.0f, label_at.y + 2.0f), Color(190, 95, 95), "X");
        if (ProjectToViewport(vp, Vec3(0.0f, 0.0f, label_distance), origin, size, label_at)) draw->AddText(ImVec2(label_at.x + 3.0f, label_at.y + 2.0f), Color(100, 135, 200), "Z");
    }

    // Each entity with a Transform: its box (the Transform's scale is the box's size) in its kind's colour, orange when
    // selected and white when hovered; a marker kind also gets an icon at its origin and its name, so a camera, a light
    // and a player are not three identical grey boxes.
    const World& world = document.GetWorld();
    Entity hovered_entity = kNullEntity;
    if (hovered && !over_chrome) hovered_entity = PickEntity(document, camera_, origin, size, io.MousePos, 14.0f, bounds);
    bool kind_present[11] = {};
    for (const Entity entity : document.Entities()) {
        if (!world.HasComponent<Transform>(entity) || document.IsHidden(entity)) continue;
        const Mat4 m = ComputeWorldTransform(world, document.Guids(), entity);
        const bool selected = document.IsSelected(entity);
        const EntityKind kind = ClassifyEntity(world, entity);
        kind_present[static_cast<int>(kind)] = true;
        const ImU32 color = selected ? Color(255, 170, 40)
                                     : (entity == hovered_entity ? Color(255, 255, 255) : ImGui::GetColorU32(EntityKindColor(kind)));
        // Over a rendered picture only the selected (and the hovered) are boxed: every entity's box over the lit scene would hide it.
        if (show.bounds && (!shaded || selected || entity == hovered_entity)) {
            ViewportTools::DrawEntityBox(draw, document, camera_, origin, size, entity, color, selected ? 2.0f : 1.0f, bounds);
        }
        const Vec3 origin_w(m.cols[3].x, m.cols[3].y, m.cols[3].z);
        if (show.helpers && selected) { // the axes of what is selected, in its own orientation
            const Vec3 x(m.cols[0].x, m.cols[0].y, m.cols[0].z), y(m.cols[1].x, m.cols[1].y, m.cols[1].z), z(m.cols[2].x, m.cols[2].y, m.cols[2].z);
            const f32 lx = std::max(x.Length(), 1e-4f), ly = std::max(y.Length(), 1e-4f), lz = std::max(z.Length(), 1e-4f);
            line(origin_w, origin_w + x * (1.2f / lx), Color(230, 70, 70), 2.0f);
            line(origin_w, origin_w + y * (1.2f / ly), Color(90, 210, 90), 2.0f);
            line(origin_w, origin_w + z * (1.2f / lz), Color(80, 120, 240), 2.0f);
        }
        ImVec2 at;
        const bool projected = ProjectToViewport(vp, origin_w, origin, size, at);
        if (show.helpers && projected) {
            DrawKindIcon(draw, at, kind, color);
            // A model is its box; the other kinds are markers, so they are always named (the selected and hovered too).
            if (selected || entity == hovered_entity || kind != EntityKind::Model) {
                draw->AddText(ImVec2(at.x + 9.0f, at.y - 18.0f), color, document.Name(entity).c_str());
            }
            // A camera shows the frustum it sees through and a light the volume or direction it casts: what it does, not
            // just where it is.
            if (kind == EntityKind::Camera || kind == EntityKind::Light) DrawKindGizmo(document, entity, kind, line, color);
        }
    }
    // What the colours mean, so a box's colour says what it is without selecting it. Only the kinds in the scene.
    if (show.helpers) {
        usize rows = 0;
        for (const KindLegendEntry& entry : kLegend)
            if (kind_present[static_cast<int>(entry.kind)]) ++rows;
        if (rows > 0) {
            const f32 line_height = ImGui::GetTextLineHeight();
            const f32 width = 96.0f;
            const ImVec2 low(end.x - width - 10.0f, origin.y + kChipHeight + 18.0f); // under the top-right chips
            const ImVec2 high(end.x - 10.0f, low.y + static_cast<f32>(rows) * line_height + 10.0f);
            draw->AddRectFilled(low, high, Color(18, 20, 25, 205), 4.0f);
            f32 y = low.y + 5.0f;
            for (const KindLegendEntry& entry : kLegend) {
                if (!kind_present[static_cast<int>(entry.kind)]) continue;
                draw->AddCircleFilled(ImVec2(low.x + 8.0f, y + line_height * 0.5f), 3.0f, ImGui::GetColorU32(EntityKindColor(entry.kind)));
                draw->AddText(ImVec2(low.x + 16.0f, y), Color(200, 205, 214), entry.label);
                y += line_height;
            }
        }
    }
    if (show.collision) DrawColliders(document, camera_, origin, size, line);
    drop_.DrawPreview(draw, camera_, origin, size);
    if (show.gizmos) tools_.DrawGizmo(draw, document, camera_, origin, size, hovered);
    tools_.DrawMarquee(draw, origin, size);
    // A short message in the middle: the fly speed while the wheel is changing it, otherwise a bookmark that was saved or recalled.
    const std::string message = speed_notice_ > 0.0f ? "Speed " + std::to_string(static_cast<int>(std::lround(camera_.fly_speed))) + " m/s"
                                                     : (notice_left_ > 0.0f ? notice_ : std::string());
    if (!message.empty()) {
        const ImVec2 extent = ImGui::CalcTextSize(message.c_str());
        draw->AddText(ImVec2(origin.x + (size.x - extent.x) * 0.5f, origin.y + 14.0f), Color(235, 238, 245, 220), message.c_str());
    }
    // The chips last, above everything the picture holds: the viewport's name and look, the gizmo's space, snapping and
    // the grid, the selection and the counts.
    DrawChip(draw, viewport_label, "Viewport 1", false);
    DrawChip(draw, projection_chip, drawn_projection, true, true);
    DrawChip(draw, view_mode_chip, drawn_mode, true, true);
    DrawChip(draw, show_chip, "Show", true, true);
    DrawChip(draw, space_chip, tools_.Space() == GizmoSpace::Local ? "Local" : "World", true);
    DrawChip(draw, snap_chip, snap_label().c_str(), true);
    DrawChip(draw, grid_chip, show.grid ? "Grid on" : "Grid off", true);
    DrawChip(draw, selection_chip, selection_text.c_str(), false);
    DrawChip(draw, stats_chip, stats_text, false);
    if (ImGui::BeginPopup("##scene_view_show_flags")) {
        ImGui::Checkbox("Grid", &show.grid);
        ImGui::Checkbox("Gizmos", &show.gizmos);
        ImGui::Checkbox("Bounds", &show.bounds);
        ImGui::Checkbox("Helpers", &show.helpers);
        ImGui::Checkbox("Collision", &show.collision);
        ImGui::EndPopup();
    }
    if (show.helpers) {
        draw->AddText(ImVec2(origin.x + 10.0f, bottom_y - ImGui::GetTextLineHeight() - 4.0f), Color(128, 135, 148),
                      "right-drag: look + WASD/QE: fly   middle-drag / shift+drag: pan   wheel: zoom (speed flying)   click: select   F: focus   1-9 / Ctrl+1-9: bookmarks");
    }
    draw->PopClipRect();
}

} // namespace aether::editor
