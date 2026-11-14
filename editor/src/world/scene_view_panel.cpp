#include "world/scene_view_panel.h"
#include "world/content_drop.h"

#include "aether/reflection/registry.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "content/content_drag.h"
#include "core/snapping.h"

#include <algorithm>
#include <cmath>
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
        if (!world.HasComponent<Transform>(entity)) continue;
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
    ImGui::InvisibleButton("##scene_view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    // An asset dragged in from the Content Browser: a model or a prefab is placed on the ground under the mouse (rounded to the move step if Snap is on).
    drop_.Accept(document, content_root, camera_, origin, size, tools_.MoveSnapStep());
    const ImGuiIO& io = ImGui::GetIO();

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
    tools_.HandleInput(document, camera_, origin, size, hovered, active);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
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

    // Each entity with a Transform: its box (the Transform's scale is the box's size), orange when selected, cyan for a camera.
    const World& world = document.GetWorld();
    Entity hovered_entity = kNullEntity;
    if (hovered) hovered_entity = PickEntity(document, camera_, origin, size, io.MousePos, 14.0f, bounds);
    for (const Entity entity : document.Entities()) {
        if (!world.HasComponent<Transform>(entity)) continue;
        const Mat4 m = ComputeWorldTransform(world, document.Guids(), entity);
        const bool selected = document.IsSelected(entity);
        const bool is_camera = world.HasComponent<::aether::Camera>(entity);
        const ImU32 color = selected ? Color(255, 170, 40) : (entity == hovered_entity ? Color(255, 255, 255) : (is_camera ? Color(80, 200, 220) : Color(150, 158, 175)));
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
        if (show.helpers && (selected || entity == hovered_entity)) {
            ImVec2 at;
            if (ProjectToViewport(vp, origin_w, origin, size, at)) draw->AddText(ImVec2(at.x + 8.0f, at.y - 18.0f), color, document.Name(entity).c_str());
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
    if (show.helpers) {
        draw->AddText(ImVec2(origin.x + 8.0f, end.y - 22.0f), Color(128, 135, 148),
                      "right-drag: look + WASD/QE: fly   middle-drag / shift+drag: pan   wheel: zoom (speed flying)   click: select   F: focus   1-9 / Ctrl+1-9: bookmarks");
    }
    draw->PopClipRect();
}

} // namespace aether::editor
