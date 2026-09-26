// Phase 5 editor: a Dear ImGui overlay on top of the Phase 3/4 D3D12
// platform layer, driving a live ECS + Jolt physics simulation. Every body
// is an entity with Transform + RigidBody; the render loop reads Transform
// each frame to place an instanced colored quad — no bindless textures or
// GPU-driven culling here (Phase 4 already proved those out), just plain
// DrawInstanced, so the new surface area this phase adds (ImGui, physics,
// serialization) stays easy to follow.
//
// Panels: live entity list, spawn/reset controls, play/pause, and
// Save/Load Scene buttons exercising the Phase 5 serializer. Loading
// reconnects each RigidBody entity to a freshly-created Jolt body (the
// serializer deliberately does not — and cannot — persist a live body
// handle; see aether::RegisterPhysicsComponentSerializers).
//
// Editor-workflow follow-up: a loaded glTF model is now a normal ECS
// entity (Transform + ModelRenderer, see below), not a hardcoded, always-
// present render path bolted onto the side — it shows up in the same
// entity list the physics spheres do, spawns via an "Asset Browser" panel
// that lists whatever .gltf files sit under assets/models/ instead of a
// path baked into source, round-trips through Save/Load Scene exactly like
// a RigidBody entity does (ModelRenderer's asset_path is plain fixed-size
// data, so it needs no custom serializer — see GltfCache's comment), and
// can be selected for a dedicated Inspector view and an on-screen highlight
// tint.
//
// Viewport/gizmo/hierarchy follow-up: the camera is a real fly camera
// (EditorCamera — hold Right Mouse + WASD/QE), replacing the old static
// LookAtRH. Selection is now a real click-in-viewport pick (screen-to-world
// ray vs. each entity's bounding sphere — ScreenPointToRay/
// RaySphereIntersect), in addition to the list-based "Select" buttons.
// The selected entity gets a draggable 3-axis translate gizmo (line-list
// renderer, no depth test, axis-constrained drag via
// ClosestPointOnAxisToRay). A Parent component plus ComputeWorldTransform
// give entities a real scene hierarchy, shown as a tree in the Hierarchy
// panel ("Parent to selection" / "Unparent" per row).
//
// Set AETHER_EDITOR_MAX_FRAMES=<N> to auto-close after N frames instead of
// waiting for the window to be closed, for scripted/automated verification.
// Set AETHER_EDITOR_SCREENSHOT=<path> to dump the final frame to a PNG on
// exit, for visual verification without a human watching the window live.

#include "aether/assets/asset_manager.h"
#include "aether/assets/gltf_loader.h"
#include "aether/core/log.h"
#include "aether/gfx/buffer.h"
#include "aether/gfx/command_list.h"
#include "aether/gfx/descriptor_heap.h"
#include "aether/gfx/device.h"
#include "aether/gfx/material.h"
#include "aether/gfx/render_graph.h"
#include "aether/gfx/shader_compiler.h"
#include "aether/gfx/swap_chain.h"
#include "aether/job/job_system.h"
#include "aether/math/math.h"
#include "aether/physics/physics_world.h"
#include "aether/platform/window.h"
#include "aether/scene/components.h"
#include "aether/scene/serialization.h"
#include "core/commands.h"
#include "ui/entity_inspector.h"
#include "ui/reflected_inspector.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace aether;
using namespace aether::gfx;

// imgui_impl_win32.h intentionally comments this declaration out (to avoid
// forcing <windows.h> on everyone who includes it) and asks callers to copy
// it in verbatim.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr u32 kMaxInstances = 256;
constexpr const char* kScenePath = "editor_scene.aesc";

// --------------------------------------------------------------------------
// UI-workflow follow-up: a friendlier default look and a real default dock
// layout, addressing the two biggest complaints a first-time user of the
// earlier screenshots would have — "everything is default ImGui gray" and
// "every panel is stacked on top of every other panel at startup."
// --------------------------------------------------------------------------

// A soft blue-teal accent instead of ImGui's default blue, plus a bit more
// breathing room between widgets — small changes, but they're most of the
// difference between "looks like a debug overlay" and "looks like a tool."
void ApplyFriendlyEditorStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.WindowPadding = ImVec2(10, 10);
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 14.0f;

    ImVec4* colors = style.Colors;
    const ImVec4 kAccent(0.26f, 0.59f, 0.62f, 1.00f);
    const ImVec4 kAccentHover(0.32f, 0.70f, 0.74f, 1.00f);
    const ImVec4 kAccentActive(0.20f, 0.48f, 0.51f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = kAccentActive;
    colors[ImGuiCol_Header] = kAccent;
    colors[ImGuiCol_HeaderHovered] = kAccentHover;
    colors[ImGuiCol_HeaderActive] = kAccentActive;
    colors[ImGuiCol_Button] = kAccentActive;
    colors[ImGuiCol_ButtonHovered] = kAccentHover;
    colors[ImGuiCol_ButtonActive] = kAccent;
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.30f, 0.33f, 1.00f);
    colors[ImGuiCol_CheckMark] = kAccentHover;
    colors[ImGuiCol_SliderGrab] = kAccent;
    colors[ImGuiCol_SliderGrabActive] = kAccentHover;
    colors[ImGuiCol_Tab] = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    colors[ImGuiCol_TabHovered] = kAccentHover;
    colors[ImGuiCol_TabActive] = kAccentActive;
}

// Default panel layout: this editor's vendored ImGui is built from a plain
// release tag (v1.92.9), not the separate "docking" branch, so there's no
// DockBuilder/DockSpace here — instead, every panel below gets an explicit
// default position/size via ImGuiCond_FirstUseEver, arranged as a simple
// non-overlapping grid (main panel + Hierarchy + Asset Browser stacked down
// the left edge, Inspector on the right) so nothing starts stacked on top
// of anything else. ImGuiCond_FirstUseEver means this only ever applies
// before a window has a saved position — once a user drags a panel
// themselves, that layout persists via ImGui's own imgui.ini, same as any
// other ImGui app.
constexpr f32 kMenuBarHeight = 20.0f;
constexpr f32 kPanelMargin = 8.0f;
constexpr f32 kLeftPanelWidth = 340.0f;

// ModelRenderer (a model-carrying entity's glTF asset path) and SetModelPath
// now live in aether/scene/components.h, reflected, since they're runtime
// components rather than editor ones.

// World::ForEach/ForEachChunk give component references but not the Entity
// each one belongs to — fine for the physics sync system, not enough for an
// editor that needs to select/delete a specific entity by identity. Built on
// the same type-erased Archetype access the scene serializer uses (see
// World::ForEachArchetype's comment), just paired with EntityArray().
template <typename... Components, typename Func>
void ForEachWithEntity(World& world, Func&& func) {
    ComponentMask query_mask = ComponentMaskOf<Components...>();
    world.ForEachArchetype([&](Archetype& archetype) {
        if ((archetype.Mask() & query_mask) != query_mask) {
            return;
        }
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            u32 count = archetype.ChunkEntityCount(c);
            if (count == 0) {
                continue;
            }
            Entity* entities = archetype.EntityArray(c);
            std::tuple<Components*...> arrays{
                static_cast<Components*>(archetype.ComponentArray(c, GetComponentId<Components>()))...};
            for (u32 i = 0; i < count; ++i) {
                std::apply([&](Components*... arr) { func(entities[i], arr[i]...); }, arrays);
            }
        }
    });
}

// Scene-hierarchy follow-up: an entity's Transform is local to its parent
// (if any) instead of always being world space. A flat ECS has no built-in
// notion of "child of" — this is the one component that adds it. Walked in
// ComputeWorldTransform below; bounded to avoid spinning forever if a save
// file (or a bug) ever produces a cycle.
struct Parent {
    Entity entity = kNullEntity;
};

Mat4 LocalTransformMatrix(const Transform& t) { return Mat4::Translation(t.position) * t.rotation.ToMat4(); }

// Composes local -> world by walking the Parent chain. Every render/pick/
// gizmo site that used to read Transform directly and treat it as world
// space now goes through this instead, so parenting an entity actually
// moves it (and its children) instead of only affecting a tree-view label.
Mat4 ComputeWorldTransform(World& world, Entity e) {
    Mat4 result = Mat4::Identity();
    Entity current = e;
    for (int guard = 0; guard < 32 && world.IsAlive(current); ++guard) {
        Transform* t = world.GetComponent<Transform>(current);
        if (!t) {
            break;
        }
        result = LocalTransformMatrix(*t) * result;
        Parent* parent = world.GetComponent<Parent>(current);
        current = parent ? parent->entity : kNullEntity;
    }
    return result;
}

Vec3 WorldPosition(World& world, Entity e) {
    Mat4 world_transform = ComputeWorldTransform(world, e);
    return Vec3(world_transform.cols[3].x, world_transform.cols[3].y, world_transform.cols[3].z);
}

// Parent still stores a raw Entity handle (it moves to EntityGuid with the
// reparent command, Phase 7 step 4), so undo/redo — which destroys and
// recreates entities with new handles — can leave a link pointing at a dead
// entity. Drop those links instead of following them.
void RemoveStaleParents(World& world) {
    std::vector<Entity> stale;
    ForEachWithEntity<Parent>(world, [&](Entity child, Parent& p) {
        if (!world.IsAlive(p.entity)) {
            stale.push_back(child);
        }
    });
    for (Entity child : stale) {
        world.RemoveComponent<Parent>(child);
    }
}

// Rejects a would-be parent assignment that would create a cycle (making e
// its own ancestor) — the one invariant the tree-view UI below relies on to
// never infinite-loop.
bool WouldCreateCycle(World& world, Entity e, Entity new_parent) {
    Entity current = new_parent;
    for (int guard = 0; guard < 32 && world.IsAlive(current); ++guard) {
        if (current == e) {
            return true;
        }
        Parent* parent = world.GetComponent<Parent>(current);
        current = parent ? parent->entity : kNullEntity;
    }
    return false;
}

// --------------------------------------------------------------------------
// Editor camera: replaces the hardcoded static LookAtRH(0,6,-14 -> 0,1,0)
// view with a real fly camera, since viewport picking and a gizmo are both
// meaningless against a viewpoint the user can't move. Standard Unity/
// Unreal-style scheme: hold the right mouse button to look around (mouse
// delta -> yaw/pitch) and move with WASD/QE while it's held. Gated on
// !WantCaptureMouse so dragging an ImGui slider or panel never also spins
// the camera.
// --------------------------------------------------------------------------
struct EditorCamera {
    Vec3 position{0.0f, 6.0f, -14.0f};
    f32 yaw = 0.0f;     // radians, around world +Y
    f32 pitch = -0.343f; // radians, matches the old static camera's framing
    f32 move_speed = 6.0f;
    f32 look_speed = 0.0025f;

    Vec3 Forward() const {
        return Vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)).Normalized();
    }
    Vec3 Right() const { return Forward().Cross(Vec3(0, 1, 0)).Normalized(); }
    Mat4 ViewMatrix() const { return Mat4::LookAtRH(position, position + Forward(), Vec3(0, 1, 0)); }

    void Update(f32 dt) {
        ImGuiIO& io = ImGui::GetIO();
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) || io.WantCaptureMouse) {
            return;
        }
        yaw += io.MouseDelta.x * look_speed;
        pitch -= io.MouseDelta.y * look_speed;
        constexpr f32 kPitchLimit = 1.5f;
        pitch = std::clamp(pitch, -kPitchLimit, kPitchLimit);

        Vec3 forward = Forward();
        Vec3 right = Right();
        f32 amount = move_speed * dt;
        if (ImGui::IsKeyDown(ImGuiKey_W)) position = position + forward * amount;
        if (ImGui::IsKeyDown(ImGuiKey_S)) position = position - forward * amount;
        if (ImGui::IsKeyDown(ImGuiKey_D)) position = position + right * amount;
        if (ImGui::IsKeyDown(ImGuiKey_A)) position = position - right * amount;
        if (ImGui::IsKeyDown(ImGuiKey_E)) position.y += amount;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) position.y -= amount;
    }
};

// --------------------------------------------------------------------------
// Viewport picking + gizmo math: a screen-space mouse position becomes a
// world-space ray (ScreenPointToRay), tested against each candidate
// entity's bounding sphere (RaySphereIntersect) to find what's under the
// cursor; WorldToScreen does the reverse projection for gizmo hit-testing;
// ClosestPointOnAxisToRay is what makes dragging a gizmo arrow move the
// entity strictly along that one world-space axis instead of snapping to
// wherever the mouse ray happens to be.
// --------------------------------------------------------------------------
// General 4x4 inverse (the public-domain MESA/GLU cofactor-expansion
// algorithm) — Mat4 only ships the handful of constructors it needs
// elsewhere (Translation/Scale/LookAtRH/PerspectiveRH), none of which
// require inverting an arbitrary matrix, so view_proj's inverse (needed to
// unproject a mouse click into a world-space ray) is built here instead.
bool InvertMatrix4x4(const f32 m[16], f32 inv_out[16]) {
    f32 inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    f32 det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        return false;
    }
    f32 inv_det = 1.0f / det;
    for (int i = 0; i < 16; ++i) {
        inv_out[i] = inv[i] * inv_det;
    }
    return true;
}

Mat4 InverseGeneral(const Mat4& m) {
    f32 a[16];
    for (int c = 0; c < 4; ++c) {
        a[c * 4 + 0] = m.cols[c].x;
        a[c * 4 + 1] = m.cols[c].y;
        a[c * 4 + 2] = m.cols[c].z;
        a[c * 4 + 3] = m.cols[c].w;
    }
    f32 inv[16];
    if (!InvertMatrix4x4(a, inv)) {
        return Mat4::Identity();
    }
    Mat4 result;
    for (int c = 0; c < 4; ++c) {
        result.cols[c] = Vec4(inv[c * 4 + 0], inv[c * 4 + 1], inv[c * 4 + 2], inv[c * 4 + 3]);
    }
    return result;
}

struct Ray {
    Vec3 origin;
    Vec3 dir; // normalized
};

Ray ScreenPointToRay(f32 mouse_x, f32 mouse_y, u32 width, u32 height, const Mat4& view_proj) {
    f32 ndc_x = (mouse_x / static_cast<f32>(width)) * 2.0f - 1.0f;
    f32 ndc_y = 1.0f - (mouse_y / static_cast<f32>(height)) * 2.0f;

    Mat4 inv_view_proj = InverseGeneral(view_proj);
    Vec4 near_h = inv_view_proj * Vec4(ndc_x, ndc_y, 0.0f, 1.0f);
    Vec4 far_h = inv_view_proj * Vec4(ndc_x, ndc_y, 1.0f, 1.0f);
    near_h = near_h * (1.0f / near_h.w);
    far_h = far_h * (1.0f / far_h.w);

    Vec3 origin(near_h.x, near_h.y, near_h.z);
    Vec3 far_point(far_h.x, far_h.y, far_h.z);
    return Ray{origin, (far_point - origin).Normalized()};
}

bool RaySphereIntersect(const Ray& ray, Vec3 center, f32 radius, f32& out_t) {
    Vec3 oc = ray.origin - center;
    f32 b = oc.Dot(ray.dir);
    f32 c = oc.Dot(oc) - radius * radius;
    f32 disc = b * b - c;
    if (disc < 0.0f) {
        return false;
    }
    f32 sqrt_disc = std::sqrt(disc);
    f32 t = -b - sqrt_disc;
    if (t < 0.0f) {
        t = -b + sqrt_disc;
    }
    if (t < 0.0f) {
        return false;
    }
    out_t = t;
    return true;
}

struct ScreenPos {
    f32 x, y;
    bool visible;
};

ScreenPos WorldToScreen(Vec3 world, const Mat4& view_proj, u32 width, u32 height) {
    Vec4 clip = view_proj * Vec4(world.x, world.y, world.z, 1.0f);
    if (clip.w <= 0.0001f) {
        return {0, 0, false};
    }
    Vec3 ndc(clip.x / clip.w, clip.y / clip.w, clip.z / clip.w);
    return {(ndc.x * 0.5f + 0.5f) * static_cast<f32>(width), (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<f32>(height),
            true};
}

f32 PointSegmentDistance2D(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by) {
    f32 dx = bx - ax, dy = by - ay;
    f32 len_sq = dx * dx + dy * dy;
    f32 t = len_sq > 1e-6f ? std::clamp(((px - ax) * dx + (py - ay) * dy) / len_sq, 0.0f, 1.0f) : 0.0f;
    f32 cx = ax + t * dx, cy = ay + t * dy;
    return std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

// Closest point on the infinite line (p0 + t*axis) to the ray (origin,
// dir) — the standard closest-point-between-two-lines formula, specialized
// for a unit axis direction. Used so an axis-constrained gizmo drag tracks
// the mouse correctly in 3D regardless of camera angle, instead of a naive
// (and wrong) screen-space-only projection.
Vec3 ClosestPointOnAxisToRay(Vec3 p0, Vec3 axis, const Ray& ray) {
    Vec3 w0 = p0 - ray.origin;
    f32 a = 1.0f; // axis.Dot(axis), axis is unit length
    f32 b = axis.Dot(ray.dir);
    f32 c = 1.0f; // ray.dir.Dot(ray.dir), dir is unit length
    f32 d = axis.Dot(w0);
    f32 e = ray.dir.Dot(w0);
    f32 denom = a * c - b * b;
    f32 t = (std::fabs(denom) > 1e-6f) ? (b * e - c * d) / denom : 0.0f;
    return p0 + axis * t;
}

struct EditorInstance {
    f32 center[3];
    f32 radius;
    f32 color[3];
    f32 pad;
};

constexpr const char* kShaderSource = R"(
struct InstanceData {
    float3 center;
    float radius;
    float3 color;
    float _pad;
};
StructuredBuffer<InstanceData> g_Instances : register(t0);
cbuffer ViewProj : register(b0) { float4x4 g_ViewProj; };

struct PSInput {
    float4 position : SV_POSITION;
    float3 color : COLOR;
};

static const float2 kQuadPositions[6] = {
    float2(-0.5, -0.5), float2(-0.5, 0.5), float2(0.5, -0.5),
    float2(0.5, -0.5), float2(-0.5, 0.5), float2(0.5, 0.5),
};

PSInput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    InstanceData inst = g_Instances[instanceID];
    float2 localPos = kQuadPositions[vertexID] * inst.radius;
    float3 worldPos = inst.center + float3(localPos, 0.0);

    PSInput result;
    result.position = mul(g_ViewProj, float4(worldPos, 1.0));
    result.color = inst.color;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return float4(input.color, 1.0);
}
)";

ComPtr<ID3D12RootSignature> CreateRootSignature(Device& device) {
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor = {0, 0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("D3D12", "Root signature serialization failed: %s", message);
        throw std::runtime_error("root signature serialization failed");
    }
    ComPtr<ID3D12RootSignature> root_signature;
    AETHER_D3D_CHECK(device.Handle()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature)));
    return root_signature;
}

ComPtr<ID3D12PipelineState> CreatePSO(Device& device, ID3D12RootSignature* root_signature, DXGI_FORMAT rtv_format) {
    ShaderBytecode vs = CompileHLSL(kShaderSource, "VSMain", "vs_5_0", "editor_vs");
    ShaderBytecode ps = CompileHLSL(kShaderSource, "PSMain", "ps_5_0", "editor_ps");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.VS = {vs.Data(), vs.Size()};
    desc.PS = {ps.Data(), ps.Size()};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // Depth-tested (was DepthEnable=FALSE before the glTF integration
    // follow-up added a real 3D mesh into the same scene): the billboard
    // spheres and the glTF model now need to occlude each other correctly
    // by actual depth, not just by draw order.
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = rtv_format;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    AETHER_D3D_CHECK(device.Handle()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    return pso;
}

// --------------------------------------------------------------------------
// glTF integration: loads real glTF assets (material + animation) into the
// same scene the ECS/physics entities above live in — connecting the asset
// pipeline gltf_demo already proved out to the actual gameplay editor, not
// just another standalone demo. Shading/root-signature/vertex-layout is the
// same shape gltf_demo uses (Lambertian + Blinn-Phong, bindless-texture
// material system via AssetManager/LoadMaterial), duplicated here rather
// than shared through a header: both are still demo-scale single-file
// programs, and factoring a shared renderer out is a larger refactor than
// this follow-up's scope.
//
// Editor-workflow follow-up: a model is now a ModelRenderer component on a
// normal entity (see its own comment above) rather than one hardcoded,
// always-present model — GltfRenderData/GltfCache below hold the actual
// GPU/CPU resources, keyed by asset path and shared across every entity
// referencing the same one, since a component can't itself own GPU buffers
// or a std::string account for its unpredictable size in the ECS's fixed-
// slot chunked storage.
// --------------------------------------------------------------------------

constexpr u32 kGltfBindlessCapacity = 16;

constexpr const char* kGltfShaderSource = R"(
cbuffer FrameConstants : register(b0) {
    float4x4 g_MVP;
    float4x4 g_Model;
    float3 g_CameraPos;
    float _pad0;
    float3 g_LightDir;
    float _pad1;
    // Selection follow-up: >0.5 when this draw's entity is the editor's
    // selected_entity, tinting toward gold in PSMain — the "click-to-select"
    // workflow's visual feedback (selection itself is a "Select" button per
    // entity-list row, not yet a true click-in-viewport pick).
    float g_Highlight;
    float3 _pad2;
};

cbuffer MaterialConstants : register(b1) {
    float4 g_BaseColorFactor;
    float g_Metallic;
    float g_Roughness;
    uint g_BaseColorTexture;
    uint g_NormalTexture;
    uint g_MetallicRoughnessTexture;
};

Texture2D g_Textures[16] : register(t0, space1);
SamplerState g_Sampler : register(s0);

static const uint kInvalidTextureIndex = 0xFFFFFFFF;

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD1;
};

PSInput VSMain(VSInput input) {
    PSInput result;
    float4 worldPos = mul(g_Model, float4(input.position, 1.0));
    result.worldPos = worldPos.xyz;
    result.position = mul(g_MVP, float4(input.position, 1.0));
    result.normal = mul((float3x3)g_Model, input.normal);
    result.uv = input.uv;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 N = normalize(input.normal);
    float3 L = normalize(-g_LightDir);
    float3 V = normalize(g_CameraPos - input.worldPos);
    float3 H = normalize(V + L);

    float3 albedo = g_BaseColorFactor.rgb;
    if (g_BaseColorTexture != kInvalidTextureIndex) {
        albedo *= g_Textures[g_BaseColorTexture].Sample(g_Sampler, input.uv).rgb;
    }

    float ambient = 0.15;
    float diffuse = max(dot(N, L), 0.0);
    float specular = pow(max(dot(N, H), 0.0), 32.0) * (1.0 - g_Roughness) * 0.5;

    float3 color = albedo * (ambient + diffuse) + float3(1.0, 1.0, 1.0) * specular;
    color = lerp(color, float3(1.0, 0.85, 0.2), g_Highlight * 0.6);
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);
    return float4(color, 1.0);
}
)";

struct GltfFrameConstants {
    Mat4 mvp;
    Mat4 model;
    Vec3 camera_pos;
    f32 pad0;
    Vec3 light_dir;
    f32 pad1;
    f32 highlight;
    Vec3 pad2;
};

struct GltfSimpleVertex {
    f32 pos[3];
    f32 normal[3];
    f32 uv[2];
};

ComPtr<ID3D12RootSignature> CreateGltfRootSignature(Device& device) {
    D3D12_DESCRIPTOR_RANGE bindless_range{};
    bindless_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindless_range.NumDescriptors = kGltfBindlessCapacity;
    bindless_range.BaseShaderRegister = 0;
    bindless_range.RegisterSpace = 1;
    bindless_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0,
                            /*Num32BitValues=*/sizeof(GltfFrameConstants) / 4};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {/*ShaderRegister=*/1, /*RegisterSpace=*/0, /*Num32BitValues=*/sizeof(MaterialData) / 4};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {1, &bindless_range};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("Editor", "glTF root signature serialization failed: %s", message);
        throw std::runtime_error("gltf root signature serialization failed");
    }
    ComPtr<ID3D12RootSignature> root_signature;
    AETHER_D3D_CHECK(device.Handle()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature)));
    return root_signature;
}

ComPtr<ID3D12PipelineState> CreateGltfPSO(Device& device, ID3D12RootSignature* root_signature,
                                           DXGI_FORMAT rtv_format) {
    ShaderBytecode vs = CompileHLSL(kGltfShaderSource, "VSMain", "vs_5_1", "editor_gltf_vs");
    ShaderBytecode ps = CompileHLSL(kGltfShaderSource, "PSMain", "ps_5_1", "editor_gltf_ps");

    D3D12_INPUT_ELEMENT_DESC input_elements[3] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GltfSimpleVertex, pos),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GltfSimpleVertex, normal),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(GltfSimpleVertex, uv),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.InputLayout = {input_elements, 3};
    desc.VS = {vs.Data(), vs.Size()};
    desc.PS = {ps.Data(), ps.Size()};

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.DepthClipEnable = TRUE;

    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = FALSE;

    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = rtv_format;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    AETHER_D3D_CHECK(device.Handle()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    return pso;
}

// --------------------------------------------------------------------------
// Transform gizmo: a minimal unlit line-list renderer (no depth test, so the
// handles always draw on top of the scene, same as every other 3D editor's
// gizmo) for the three translate-axis handles drawn at the selected
// entity's position. Deliberately its own tiny pipeline rather than reusing
// the sphere or glTF ones — neither is a colored-line renderer, and this one
// needs no textures, no lighting, and writes a fresh 6-vertex buffer once
// per frame (cheap enough not to double-buffer).
// --------------------------------------------------------------------------
struct GizmoVertex {
    f32 pos[3];
    f32 color[3];
};

constexpr const char* kGizmoShaderSource = R"(
cbuffer ViewProj : register(b0) { float4x4 g_ViewProj; };

struct VSInput {
    float3 position : POSITION;
    float3 color : COLOR;
};
struct PSInput {
    float4 position : SV_POSITION;
    float3 color : COLOR;
};

PSInput VSMain(VSInput input) {
    PSInput result;
    result.position = mul(g_ViewProj, float4(input.position, 1.0));
    result.color = input.color;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return float4(input.color, 1.0);
}
)";

ComPtr<ID3D12RootSignature> CreateGizmoRootSignature(Device& device) {
    D3D12_ROOT_PARAMETER params[1]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    // Required because the gizmo PSO below has a real vertex input layout
    // (unlike the billboard-sphere pipeline, which reads SV_VertexID and
    // needs no vertex buffer at all) — omitting this flag is what made
    // CreateGraphicsPipelineState fail with E_INVALIDARG the first time.
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("D3D12", "Gizmo root signature serialization failed: %s", message);
        throw std::runtime_error("gizmo root signature serialization failed");
    }
    ComPtr<ID3D12RootSignature> root_signature;
    AETHER_D3D_CHECK(device.Handle()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature)));
    return root_signature;
}

ComPtr<ID3D12PipelineState> CreateGizmoPSO(Device& device, ID3D12RootSignature* root_signature,
                                            DXGI_FORMAT rtv_format) {
    ShaderBytecode vs = CompileHLSL(kGizmoShaderSource, "VSMain", "vs_5_0", "editor_gizmo_vs");
    ShaderBytecode ps = CompileHLSL(kGizmoShaderSource, "PSMain", "ps_5_0", "editor_gizmo_ps");

    D3D12_INPUT_ELEMENT_DESC input_elements[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GizmoVertex, pos),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GizmoVertex, color),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.InputLayout = {input_elements, 2};
    desc.VS = {vs.Data(), vs.Size()};
    desc.PS = {ps.Data(), ps.Size()};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // No depth test: a gizmo that could disappear behind geometry it's
    // meant to be manipulating would make dragging it unreliable — every
    // mainstream 3D editor draws gizmos on top unconditionally.
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = rtv_format;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    AETHER_D3D_CHECK(device.Handle()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    return pso;
}

// Everything needed to draw one glTF asset: geometry buffers, the resolved
// material, and the parsed GltfScene itself (kept around for its
// node_instances/animations, re-evaluated in place each frame — see the
// main loop). Cached by asset path in a std::unordered_map (GltfCache,
// below) and shared by every ModelRenderer entity that references the same
// path, the same way AssetManager already shares a decoded texture across
// materials that reference it — spawning ten entities pointing at one
// .gltf file uploads its geometry/texture exactly once. `animate`/
// `anim_time` are per-asset, not per-entity: two entities sharing one
// animated asset play in lockstep, a scope choice rather than a limitation
// worth solving before this asset actually has two independent instances
// that need to desync.
struct GltfRenderData {
    assets::GltfScene scene;
    std::unique_ptr<Buffer> vertex_buffer;
    std::unique_ptr<Buffer> index_buffer;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    MaterialData material;
    usize index_count = 0;
    bool valid = false; // false if LoadGltf failed — draw calls skip it, the UI still shows it as an entry
    bool animate = true;
    f32 anim_time = 0.0f;
    // Viewport-picking follow-up: an approximate model-space bounding sphere
    // (center + radius) computed once at load from the raw vertex extents,
    // used to hit-test this asset against a mouse ray the same way a
    // RigidBody's own `radius` already is — good enough for "is the cursor
    // roughly over this model," not a substitute for real per-triangle
    // picking.
    Vec3 bounds_center{0, 0, 0};
    f32 bounds_radius = 0.5f;
};

using GltfCache = std::unordered_map<std::string, std::unique_ptr<GltfRenderData>>;

GltfRenderData& GetOrLoadGltfRenderData(Device& device, assets::AssetManager& asset_manager, GltfCache& cache,
                                        const std::string& path) {
    auto existing = cache.find(path);
    if (existing != cache.end()) {
        return *existing->second;
    }

    auto data = std::make_unique<GltfRenderData>();
    std::string full_path = std::string(AETHER_ASSET_DIR) + path;
    bool ok = assets::LoadGltf(full_path, data->scene) && !data->scene.meshes.empty() &&
              !data->scene.meshes[0].primitives.empty();
    if (!ok) {
        AETHER_LOG_ERROR("Editor", "Failed to load glTF model \"%s\" — entity will render nothing", full_path.c_str());
        GltfRenderData& ref = *data;
        cache.emplace(path, std::move(data));
        return ref;
    }
    if (data->scene.node_instances.empty()) {
        data->scene.node_instances.push_back({0, Mat4::Identity()});
    }

    const assets::GltfPrimitive& primitive = data->scene.meshes[0].primitives[0];
    data->index_count = primitive.indices.size();

    Vec3 bounds_min(1e30f, 1e30f, 1e30f);
    Vec3 bounds_max(-1e30f, -1e30f, -1e30f);
    std::vector<GltfSimpleVertex> vertices(primitive.vertices.size());
    for (usize i = 0; i < primitive.vertices.size(); ++i) {
        std::memcpy(vertices[i].pos, primitive.vertices[i].position, sizeof(f32) * 3);
        std::memcpy(vertices[i].normal, primitive.vertices[i].normal, sizeof(f32) * 3);
        std::memcpy(vertices[i].uv, primitive.vertices[i].uv, sizeof(f32) * 2);
        Vec3 p(vertices[i].pos[0], vertices[i].pos[1], vertices[i].pos[2]);
        bounds_min = Vec3(std::min(bounds_min.x, p.x), std::min(bounds_min.y, p.y), std::min(bounds_min.z, p.z));
        bounds_max = Vec3(std::max(bounds_max.x, p.x), std::max(bounds_max.y, p.y), std::max(bounds_max.z, p.z));
    }
    if (!vertices.empty()) {
        data->bounds_center = (bounds_min + bounds_max) * 0.5f;
        data->bounds_radius = std::max((bounds_max - bounds_min).Length() * 0.5f, 0.1f);
    }
    data->vertex_buffer =
        std::make_unique<Buffer>(device, vertices.size() * sizeof(GltfSimpleVertex), BufferKind::Upload);
    data->vertex_buffer->Update(vertices.data(), vertices.size() * sizeof(GltfSimpleVertex));
    data->index_buffer = std::make_unique<Buffer>(device, data->index_count * sizeof(u32), BufferKind::Upload);
    data->index_buffer->Update(primitive.indices.data(), data->index_count * sizeof(u32));

    data->vbv.BufferLocation = data->vertex_buffer->GPUAddress();
    data->vbv.SizeInBytes = static_cast<UINT>(data->vertex_buffer->Size());
    data->vbv.StrideInBytes = sizeof(GltfSimpleVertex);
    data->ibv.BufferLocation = data->index_buffer->GPUAddress();
    data->ibv.SizeInBytes = static_cast<UINT>(data->index_buffer->Size());
    data->ibv.Format = DXGI_FORMAT_R32_UINT;

    CommandList setup_cmd(device);
    setup_cmd.Reset();
    if (primitive.material_index >= 0 &&
        static_cast<usize>(primitive.material_index) < data->scene.materials.size()) {
        data->material =
            LoadMaterial(data->scene.materials[primitive.material_index], asset_manager, setup_cmd.Get());
    }
    setup_cmd.Close();
    ID3D12CommandList* setup_lists[] = {setup_cmd.Get()};
    device.WaitForFence(device.Submit(setup_lists, 1));

    data->valid = true;
    AETHER_LOG_INFO("Editor", "Loaded glTF model \"%s\": %zu vert(s), %zu index(es), %zu animation(s)",
                     full_path.c_str(), primitive.vertices.size(), data->index_count, data->scene.animations.size());

    GltfRenderData& ref = *data;
    cache.emplace(path, std::move(data));
    return ref;
}

// Scans assets/models/ for .gltf files — the "in-editor asset picker"
// follow-up: the Add Model panel lists whatever this returns instead of a
// path hardcoded in source.
std::vector<std::string> ListAvailableGltfModels() {
    std::vector<std::string> paths;
    std::filesystem::path models_dir = std::filesystem::path(AETHER_ASSET_DIR) / "models";
    if (!std::filesystem::exists(models_dir)) {
        return paths;
    }
    for (const auto& entry : std::filesystem::directory_iterator(models_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".gltf") {
            paths.push_back("models/" + entry.path().filename().string());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

Entity SpawnSphere(World& world, PhysicsWorld& physics, const Vec3& position, f32 radius, f32 mass) {
    JPH::BodyID body_id = physics.CreateSphere(position, radius, mass, /*is_static=*/false);
    RigidBody body;
    body.body_id = body_id;
    body.radius = radius;
    body.mass = mass;
    body.is_static = false;
    return world.CreateEntity(Transform{position, Quaternion::Identity()}, body);
}

// Same DXGI-flip-model-aware backbuffer readback as gltf_demo/pbr_demo (see
// their comment on why the buffer index must be the caller-tracked "last
// rendered" one, not CurrentBackBuffer()).
void SaveBackbufferScreenshot(Device& device, SwapChain& swap_chain, u32 buffer_index, const std::string& path) {
    ID3D12Resource* back_buffer = swap_chain.BackBuffer(buffer_index);
    D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 total_bytes = 0;
    device.Handle()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);

    Buffer readback(device, total_bytes, BufferKind::Readback);

    CommandList cmd(device);
    cmd.Reset();

    D3D12_RESOURCE_BARRIER to_copy_src =
        TransitionBarrier(back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &to_copy_src);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = back_buffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Handle();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER to_present =
        TransitionBarrier(back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    cmd->ResourceBarrier(1, &to_present);

    cmd.Close();
    ID3D12CommandList* lists[] = {cmd.Get()};
    device.WaitForFence(device.Submit(lists, 1));

    std::vector<u8> raw(total_bytes);
    readback.Read(raw.data(), total_bytes);

    u32 width = static_cast<u32>(desc.Width);
    u32 height = desc.Height;
    std::vector<u8> tight(static_cast<usize>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(&tight[static_cast<usize>(y) * width * 4],
                    &raw[static_cast<usize>(y) * footprint.Footprint.RowPitch], static_cast<usize>(width) * 4);
    }

    stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, tight.data(),
                   static_cast<int>(width) * 4);
    AETHER_LOG_INFO("Editor", "Wrote screenshot to \"%s\" (%ux%u)", path.c_str(), width, height);
}

} // namespace

int main() {
    i32 max_frames = -1;
    if (const char* env = std::getenv("AETHER_EDITOR_MAX_FRAMES")) {
        max_frames = std::atoi(env);
    }
    const char* screenshot_path = std::getenv("AETHER_EDITOR_SCREENSHOT");

    try {
        RegisterPhysicsComponentSerializers();
        // Scenes saved before ModelRenderer moved out of this file's anonymous
        // namespace stored it under that type's typeid name. MSVC and the
        // Itanium ABI (GCC/Clang) spell it differently; register both so
        // those scenes still find their models.
        RegisterComponentAlias<ModelRenderer>("struct `anonymous namespace'::ModelRenderer");
        RegisterComponentAlias<ModelRenderer>("N12_GLOBAL__N_113ModelRendererE");

        WindowDesc window_desc;
        window_desc.title = "Aether Editor - Phase 5";
        window_desc.width = 1280;
        window_desc.height = 800;
        Window window(window_desc);

        Device device(/*enable_debug_layer=*/true);
        SwapChain swap_chain(device, window.NativeHandle(), window.Width(), window.Height());

        RenderGraph graph(device);

        // A real depth buffer — added alongside the glTF integration
        // follow-up's 3D mesh, since the billboard-only editor before this
        // never needed one (see CreatePSO's comment).
        auto create_depth_buffer = [&](u32 width, u32 height) {
            D3D12_RESOURCE_DESC depth_desc{};
            depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            depth_desc.Width = width;
            depth_desc.Height = height;
            depth_desc.DepthOrArraySize = 1;
            depth_desc.MipLevels = 1;
            depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
            depth_desc.SampleDesc.Count = 1;
            depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE clear_value{};
            clear_value.Format = DXGI_FORMAT_D32_FLOAT;
            clear_value.DepthStencil.Depth = 1.0f;

            return graph.CreateTransientTexture(depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value,
                                                 "EditorDepth");
        };
        RenderGraph::ResourceHandle depth_handle = create_depth_buffer(window.Width(), window.Height());

        window.on_resize = [&](u32 w, u32 h) {
            swap_chain.Resize(w, h);
            depth_handle = create_depth_buffer(w, h);
        };

        window.native_message_hook = [](void* hwnd, u32 msg, u64 wparam, i64 lparam) {
            ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), msg, static_cast<WPARAM>(wparam),
                                            static_cast<LPARAM>(lparam));
        };

        // A dedicated heap for ImGui's own SRVs (font atlas plus whatever
        // dynamic textures 1.92's texture-update system needs), kept
        // separate from any future bindless texture heap so the two
        // allocation schemes never collide. Sized generously since this
        // backend version can allocate more than just the one font
        // descriptor older versions needed.
        DescriptorHeap imgui_srv_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, /*shader_visible=*/true);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        // UI-workflow follow-up: a non-overlapping default layout for the
        // panels this editor already had (Aether Editor, Hierarchy,
        // Inspector, Asset Browser) instead of every one of them stacking on
        // top of each other at (0,0) — the biggest usability problem in
        // earlier screenshots. See each panel's SetNextWindowPos/Size call
        // below, and ApplyFriendlyEditorStyle for the accent-color/spacing
        // pass.
        ApplyFriendlyEditorStyle();
        ImGui_ImplWin32_Init(window.NativeHandle());

        ImGui_ImplDX12_InitInfo init_info{};
        init_info.Device = device.Handle();
        init_info.CommandQueue = device.Queue();
        init_info.NumFramesInFlight = static_cast<int>(swap_chain.BufferCount());
        init_info.RTVFormat = swap_chain.Format();
        init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        init_info.SrvDescriptorHeap = imgui_srv_heap.Heap();
        init_info.UserData = &imgui_srv_heap;
        init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                                             D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
            auto* heap = static_cast<DescriptorHeap*>(info->UserData);
            u32 slot = heap->Allocate();
            *out_cpu = heap->CPUHandle(slot);
            *out_gpu = heap->GPUHandle(slot);
        };
        init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE,
                                            D3D12_GPU_DESCRIPTOR_HANDLE) {
            // No-op: this demo never unloads ImGui textures during its
            // lifetime, so leaking the (tiny, fixed-capacity) heap slot back
            // to DescriptorHeap isn't needed. A long-running editor reusing
            // dynamic textures would want a real CPU<->GPU handle lookup here.
        };
        ImGui_ImplDX12_Init(&init_info);

        ComPtr<ID3D12RootSignature> root_signature = CreateRootSignature(device);
        ComPtr<ID3D12PipelineState> pso = CreatePSO(device, root_signature.Get(), swap_chain.Format());

        Buffer view_proj_buffer(device, sizeof(f32) * 16, BufferKind::Upload);
        Buffer instance_buffer(device, sizeof(EditorInstance) * kMaxInstances, BufferKind::Upload);

        World world;
        JobSystem job_system;
        PhysicsWorld physics(job_system);
        physics.CreateBox(Vec3(0, 0, 0), Vec3(10, 0.5f, 10));

        std::mt19937 rng(1234);
        std::uniform_real_distribution<f32> spread(-3.0f, 3.0f);
        std::uniform_real_distribution<f32> height(4.0f, 10.0f);
        for (int i = 0; i < 8; ++i) {
            SpawnSphere(world, physics, Vec3(spread(rng), height(rng), spread(rng)), 0.5f, 1.0f);
        }

        // glTF integration, now via the ECS: the rendering infrastructure
        // (pipeline, bindless heap, asset manager, cache) is built
        // unconditionally, since models are spawned/removed at runtime
        // rather than one being hardcoded and always present.
        ComPtr<ID3D12RootSignature> gltf_root_signature = CreateGltfRootSignature(device);
        ComPtr<ID3D12PipelineState> gltf_pso = CreateGltfPSO(device, gltf_root_signature.Get(), swap_chain.Format());
        DescriptorHeap gltf_bindless_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kGltfBindlessCapacity,
                                           /*shader_visible=*/true);
        assets::AssetManager gltf_asset_manager(device, gltf_bindless_heap);
        GltfCache gltf_cache;
        std::vector<std::string> available_gltf_models = ListAvailableGltfModels();

        // The one model the earlier glTF integration follow-up hardcoded is
        // now just the first entity spawned into a scene that can hold any
        // number of them, added/removed at runtime like the physics spheres
        // already are.
        Entity model_entity =
            world.CreateEntity(Transform{Vec3(-2.0f, 1.2f, 3.5f), Quaternion::Identity()}, ModelRenderer{});
        SetModelPath(*world.GetComponent<ModelRenderer>(model_entity), "models/test_animation.gltf");

        // Viewport-picking/gizmo follow-up: click an entity in the 3D
        // viewport (not just its row in a list) to select it, then drag one
        // of the gizmo's three axis handles to move it — see the top-of-file
        // comment for what this now covers.
        Entity selected_entity = kNullEntity;
        EditorCamera camera;

        // Phase 7: undo/redo. Every Inspector edit, spawn and delete is an
        // undoable command (editor/src/core/commands.h). The hooks keep Jolt
        // bodies in step with the ECS on every do, undo and redo — the
        // commands themselves know nothing about physics.
        GuidIndex guids;
        editor::CommandStack commands;
        editor::EditorHooks hooks;
        auto destroy_body = [&](Entity e) {
            if (RigidBody* b = world.GetComponent<RigidBody>(e); b != nullptr && !b->body_id.IsInvalid()) {
                physics.DestroyBody(b->body_id);
                b->body_id = JPH::BodyID();
            }
        };
        auto create_body = [&](Entity e) {
            RigidBody* b = world.GetComponent<RigidBody>(e);
            Transform* t = world.GetComponent<Transform>(e);
            if (b != nullptr && t != nullptr) {
                b->body_id = physics.CreateSphere(t->position, b->radius, b->mass, b->is_static);
            }
        };
        hooks.on_entity_created = [&](Entity e) { create_body(e); };
        hooks.on_entity_destroying = [&](Entity e) { destroy_body(e); };
        hooks.on_component_added = [&](Entity e, ComponentId id) {
            if (id != GetComponentId<RigidBody>()) {
                return;
            }
            if (!world.HasComponent<Transform>(e)) {
                world.AddComponent(e, Transform{}); // a body needs somewhere to live
            }
            create_body(e);
        };
        hooks.on_component_removing = [&](Entity e, ComponentId id) {
            if (id == GetComponentId<RigidBody>()) {
                destroy_body(e);
            }
        };
        hooks.on_field_changed = [&](Entity e, ComponentId id, const reflect::FieldInfo&) {
            RigidBody* b = world.GetComponent<RigidBody>(e);
            Transform* t = world.GetComponent<Transform>(e);
            if (b == nullptr || t == nullptr || b->body_id.IsInvalid()) {
                return;
            }
            if (id == GetComponentId<Transform>()) {
                physics.SetPosition(b->body_id, t->position);
            } else if (id == GetComponentId<RigidBody>()) {
                // Jolt shapes and motion type are effectively immutable once
                // a body exists; recreate it in place.
                destroy_body(e);
                create_body(e);
            }
        };
        editor::CommandContext cmd_ctx{world, guids, &hooks};
        editor::EnsureAllGuids(world, guids);

        ComPtr<ID3D12RootSignature> gizmo_root_signature = CreateGizmoRootSignature(device);
        ComPtr<ID3D12PipelineState> gizmo_pso = CreateGizmoPSO(device, gizmo_root_signature.Get(), swap_chain.Format());
        Buffer gizmo_vertex_buffer(device, sizeof(GizmoVertex) * 6, BufferKind::Upload);
        constexpr f32 kGizmoAxisLength = 1.5f;
        constexpr f32 kGizmoPickPixels = 10.0f;
        int gizmo_dragging_axis = -1; // -1 = not dragging, 0/1/2 = X/Y/Z
        Vec3 gizmo_drag_start_point{0, 0, 0};
        Vec3 gizmo_drag_start_entity_pos{0, 0, 0};

        // UI-workflow follow-up state: panel visibility is toggled from the
        // new "View" menu so a user who closes a panel can bring it back
        // without restarting; delete_confirm_target/name back a single
        // shared "Are you sure?" modal instead of every Delete button
        // deleting immediately and irreversibly.
        bool show_hierarchy = true;
        bool show_inspector = true;
        bool show_asset_browser = true;
        bool show_help = true;
        Entity delete_confirm_target = kNullEntity;
        std::string delete_confirm_name;

        bool playing = true;
        std::vector<std::unique_ptr<CommandList>> command_lists;
        std::vector<u64> frame_fences(swap_chain.BufferCount(), 0);
        for (u32 i = 0; i < swap_chain.BufferCount(); ++i) {
            command_lists.push_back(std::make_unique<CommandList>(device));
        }

        AETHER_LOG_INFO("Editor", "Entering main loop");

        i32 frame_index = 0;
        u32 last_rendered_buffer_index = 0;
        while (window.PumpMessages()) {
            if (window.IsMinimized()) {
                continue;
            }

            constexpr f32 kDt = 1.0f / 60.0f;
            if (playing) {
                SyncPhysicsToTransforms(world, physics, kDt);
            }

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // Undo/redo destroys and recreates entities, so any handle kept
            // across frames may have gone stale.
            if (!selected_entity.IsNull() && !world.IsAlive(selected_entity)) {
                selected_entity = kNullEntity;
            }
            if (!delete_confirm_target.IsNull() && !world.IsAlive(delete_confirm_target)) {
                delete_confirm_target = kNullEntity;
            }
            bool undo_requested = false;
            bool redo_requested = false;
            if (!ImGui::GetIO().WantTextInput) {
                undo_requested = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z);
                redo_requested = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
                                 ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z);
            }

            // Editor camera/viewport follow-up: computed once per frame,
            // right after NewFrame() (so camera.Update() can read this
            // frame's ImGui::GetIO() mouse/key state), and reused for both
            // this frame's picking/gizmo math below and its render pass at
            // the bottom of the loop — no more of the old hardcoded static
            // LookAtRH.
            camera.Update(kDt);
            Mat4 view = camera.ViewMatrix();
            Mat4 proj = Mat4::PerspectiveRH(
                Radians(60.0f), static_cast<f32>(swap_chain.Width()) / static_cast<f32>(swap_chain.Height()), 0.1f,
                100.0f);
            Mat4 view_proj = proj * view;

            // A real menu bar, mainly so a panel a user closes (via its
            // title-bar X) can be brought back without restarting — the
            // same "View > Panel Name" convention every docked-panel editor
            // uses. Called before the dockspace host below so the main
            // viewport's WorkPos/WorkSize already exclude this strip.
            if (ImGui::BeginMainMenuBar()) {
                if (ImGui::BeginMenu("Edit")) {
                    std::string undo_label = "Undo " + commands.UndoLabel();
                    std::string redo_label = "Redo " + commands.RedoLabel();
                    if (ImGui::MenuItem(undo_label.c_str(), "Ctrl+Z", false, commands.CanUndo())) {
                        undo_requested = true;
                    }
                    if (ImGui::MenuItem(redo_label.c_str(), "Ctrl+Y", false, commands.CanRedo())) {
                        redo_requested = true;
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("View")) {
                    ImGui::MenuItem("Hierarchy", nullptr, &show_hierarchy);
                    ImGui::MenuItem("Inspector", nullptr, &show_inspector);
                    ImGui::MenuItem("Asset Browser", nullptr, &show_asset_browser);
                    ImGui::Separator();
                    ImGui::MenuItem("Help", nullptr, &show_help);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Help")) {
                    if (ImGui::MenuItem("Show Help Window")) {
                        show_help = true;
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMainMenuBar();
            }
            if (undo_requested && commands.Undo(cmd_ctx)) {
                RemoveStaleParents(world);
            } else if (redo_requested && commands.Redo(cmd_ctx)) {
                RemoveStaleParents(world);
            }

            // Every currently-referenced model asset must be loaded before
            // this frame's draw commands are recorded — loading uploads via
            // its own short-lived command list + fence wait (see
            // GetOrLoadGltfRenderData), which can't happen from inside the
            // RenderGraph pass lambda below.
            ForEachWithEntity<ModelRenderer>(world, [&](Entity, ModelRenderer& renderer) {
                GetOrLoadGltfRenderData(device, gltf_asset_manager, gltf_cache, renderer.asset_path);
            });

            std::vector<EditorInstance> instances;
            ForEachWithEntity<Transform, RigidBody>(world, [&](Entity e, Transform& t, RigidBody& b) {
                if (instances.size() >= kMaxInstances) {
                    return;
                }
                bool is_selected = (e == selected_entity);
                EditorInstance inst{};
                inst.center[0] = t.position.x;
                inst.center[1] = t.position.y;
                inst.center[2] = t.position.z;
                inst.radius = b.radius;
                if (is_selected) {
                    inst.color[0] = 1.0f;
                    inst.color[1] = 0.85f;
                    inst.color[2] = 0.2f;
                } else {
                    inst.color[0] = 0.3f;
                    inst.color[1] = 0.6f + 0.05f * static_cast<f32>(instances.size() % 5);
                    inst.color[2] = 0.9f;
                }
                instances.push_back(inst);
            });

            // Viewport picking + gizmo drag: read right after NewFrame() but
            // before any ImGui panel for this frame is submitted —
            // io.WantCaptureMouse at this point still reflects where last
            // frame's panels were laid out, which is exactly the standard
            // ordering every ImGui-driven tool uses to tell "click hit a
            // panel" from "click hit the 3D viewport" apart.
            {
                ImGuiIO& io = ImGui::GetIO();
                bool mouse_over_viewport = !io.WantCaptureMouse;
                const Vec3 kAxes[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};

                if (gizmo_dragging_axis == -1 && !selected_entity.IsNull() && mouse_over_viewport &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    Transform* sel_t = world.GetComponent<Transform>(selected_entity);
                    if (sel_t) {
                        Vec3 gizmo_origin = world.HasComponent<ModelRenderer>(selected_entity)
                                                 ? WorldPosition(world, selected_entity)
                                                 : sel_t->position;
                        ScreenPos origin_screen =
                            WorldToScreen(gizmo_origin, view_proj, swap_chain.Width(), swap_chain.Height());
                        f32 best_dist = kGizmoPickPixels;
                        int best_axis = -1;
                        if (origin_screen.visible) {
                            for (int a = 0; a < 3; ++a) {
                                ScreenPos tip_screen = WorldToScreen(gizmo_origin + kAxes[a] * kGizmoAxisLength,
                                                                      view_proj, swap_chain.Width(),
                                                                      swap_chain.Height());
                                if (!tip_screen.visible) {
                                    continue;
                                }
                                f32 dist = PointSegmentDistance2D(io.MousePos.x, io.MousePos.y, origin_screen.x,
                                                                   origin_screen.y, tip_screen.x, tip_screen.y);
                                if (dist < best_dist) {
                                    best_dist = dist;
                                    best_axis = a;
                                }
                            }
                        }
                        if (best_axis >= 0) {
                            gizmo_dragging_axis = best_axis;
                            Ray ray = ScreenPointToRay(io.MousePos.x, io.MousePos.y, swap_chain.Width(),
                                                        swap_chain.Height(), view_proj);
                            gizmo_drag_start_point = ClosestPointOnAxisToRay(sel_t->position, kAxes[best_axis], ray);
                            gizmo_drag_start_entity_pos = sel_t->position;
                        }
                    }
                }

                if (gizmo_dragging_axis >= 0) {
                    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        Ray ray = ScreenPointToRay(io.MousePos.x, io.MousePos.y, swap_chain.Width(),
                                                    swap_chain.Height(), view_proj);
                        Vec3 current_point =
                            ClosestPointOnAxisToRay(gizmo_drag_start_entity_pos, kAxes[gizmo_dragging_axis], ray);
                        Vec3 delta = current_point - gizmo_drag_start_point;
                        Transform* sel_t = world.GetComponent<Transform>(selected_entity);
                        if (sel_t) {
                            sel_t->position = gizmo_drag_start_entity_pos + delta;
                            if (world.HasComponent<RigidBody>(selected_entity)) {
                                // Live-drag re-teleports the physics body the
                                // same way the Inspector's own DragFloat3
                                // position edit already does — see that
                                // panel's comment on why a full recreate
                                // isn't needed just to move it.
                                physics.SetPosition(world.GetComponent<RigidBody>(selected_entity)->body_id,
                                                     sel_t->position);
                            }
                        }
                    } else {
                        gizmo_dragging_axis = -1;
                    }
                } else if (mouse_over_viewport && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    // Not on a gizmo handle — fall through to real
                    // click-in-viewport picking: cast a ray from the camera
                    // through the cursor and take the nearest hit among
                    // every RigidBody's sphere and every ModelRenderer's
                    // (model-space, transformed to world) bounding sphere.
                    // Clicking empty space deselects, matching every
                    // mainstream 3D editor.
                    Ray ray = ScreenPointToRay(io.MousePos.x, io.MousePos.y, swap_chain.Width(), swap_chain.Height(),
                                                view_proj);
                    f32 best_t = 1e30f;
                    Entity best_entity = kNullEntity;
                    ForEachWithEntity<Transform, RigidBody>(world, [&](Entity e, Transform& t, RigidBody& b) {
                        f32 hit_t;
                        if (RaySphereIntersect(ray, t.position, b.radius, hit_t) && hit_t < best_t) {
                            best_t = hit_t;
                            best_entity = e;
                        }
                    });
                    ForEachWithEntity<Transform, ModelRenderer>(
                        world, [&](Entity e, Transform&, ModelRenderer& renderer) {
                            auto it = gltf_cache.find(renderer.asset_path);
                            if (it == gltf_cache.end() || !it->second->valid) {
                                return;
                            }
                            Mat4 model_world = ComputeWorldTransform(world, e);
                            Vec4 center_h = model_world * Vec4(it->second->bounds_center.x,
                                                                it->second->bounds_center.y,
                                                                it->second->bounds_center.z, 1.0f);
                            Vec3 world_center(center_h.x, center_h.y, center_h.z);
                            f32 hit_t;
                            if (RaySphereIntersect(ray, world_center, it->second->bounds_radius, hit_t) &&
                                hit_t < best_t) {
                                best_t = hit_t;
                                best_entity = e;
                            }
                        });
                    selected_entity = best_entity;
                }
            }

            std::vector<Entity> entities_to_delete;

            ImGui::SetNextWindowPos(ImVec2(kPanelMargin, kMenuBarHeight + kPanelMargin), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(kLeftPanelWidth, 340.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Aether Editor");
            ImGui::Text("Entities: %zu", world.EntityCount());
            ImGui::Checkbox("Playing", &playing);
            if (ImGui::Button("Spawn Sphere")) {
                Entity spawned = SpawnSphere(world, physics, Vec3(spread(rng), 8.0f, spread(rng)), 0.5f, 1.0f);
                commands.Record(editor::CreateEntityCommand::FromExisting(cmd_ctx, spawned, "Spawn Sphere"));
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Scene")) {
                if (SaveScene(world, kScenePath)) {
                    commands.MarkSaved();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Load Scene")) {
                World loaded;
                if (LoadScene(loaded, kScenePath)) {
                    world = std::move(loaded);
                    selected_entity = kNullEntity; // stale after a full world swap
                    // The serializer never persists a live Jolt body handle
                    // (see RegisterPhysicsComponentSerializers) — reconnect
                    // every loaded RigidBody to a fresh body in this
                    // PhysicsWorld, seeded from its Transform + saved shape.
                    // ModelRenderer needs no such reconnection: asset_path is
                    // plain data, and GetOrLoadGltfRenderData above re-loads
                    // (or finds already-cached) GPU resources for it lazily.
                    world.ForEach<Transform, RigidBody>([&](Transform& t, RigidBody& b) {
                        b.body_id = physics.CreateSphere(t.position, b.radius, b.mass, b.is_static);
                    });
                    // A different world: the old history no longer applies.
                    commands.Clear();
                    editor::EnsureAllGuids(world, guids);
                    commands.MarkSaved();
                }
            }

            ImGui::SeparatorText("Controls");
            ImGui::TextWrapped("Hold Right Mouse + WASD/QE to fly the camera. Left-click an entity (or empty "
                                "space) to select/deselect. Drag a gizmo arrow to move the selection.");

            // Bodies list: a colored bullet per row (matching the sphere's
            // own on-screen color scheme — gold when selected) instead of a
            // bare "#N", so a row's entity type/state reads at a glance
            // instead of only through its label text.
            ImGui::SeparatorText("Bodies");
            ImGui::TextDisabled("Live-edit: changes apply to the running simulation immediately.");
            int index = 0;
            ForEachWithEntity<Transform, RigidBody>(world, [&](Entity e, Transform& t, RigidBody& b) {
                ImGui::PushID(index);
                bool is_selected = (e == selected_entity);
                // Colored label instead of a plain "#N" — matches the same
                // gold-when-selected / blue-otherwise scheme the sphere's
                // own on-screen color already uses, so a row's state reads
                // at a glance. (Not a separate bullet glyph: the default
                // ImGui font only ships Basic Latin, so anything outside
                // ASCII renders as a missing-glyph box.)
                ImVec4 label_color = is_selected ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) : ImVec4(0.4f, 0.7f, 1.0f, 1.0f);
                ImGui::TextColored(label_color, "Body #%d", index);
                ImGui::SameLine();
                if (ImGui::SmallButton(is_selected ? "Selected" : "Select")) {
                    selected_entity = e;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete")) {
                    delete_confirm_target = e;
                    delete_confirm_name = "Body #" + std::to_string(index);
                }

                bool position_changed = ImGui::DragFloat3("Position (m)", &t.position.x, 0.05f);
                bool radius_changed = ImGui::DragFloat("Radius (m)", &b.radius, 0.01f, 0.05f, 5.0f, "%.2f");
                bool mass_changed = ImGui::DragFloat("Mass (kg)", &b.mass, 0.05f, 0.01f, 100.0f, "%.2f");
                bool static_changed = ImGui::Checkbox("Static (doesn't fall)", &b.is_static);

                if (position_changed) {
                    // Only re-teleport the body if the shape/mass didn't also
                    // change this frame — that branch below already recreates
                    // it at the (already updated) Transform position.
                    if (!radius_changed && !mass_changed && !static_changed) {
                        physics.SetPosition(b.body_id, t.position);
                    }
                }
                if (radius_changed || mass_changed || static_changed) {
                    // Jolt shapes and motion type are effectively immutable
                    // once a body is created; the simplest correct way to
                    // "edit" them live is to recreate the body in place.
                    physics.DestroyBody(b.body_id);
                    b.body_id = physics.CreateSphere(t.position, b.radius, b.mass, b.is_static);
                }

                ImGui::Separator();
                ImGui::PopID();
                ++index;
            });

            // Same list treatment for model entities, now that a model is a
            // normal (Transform, ModelRenderer) entity instead of a special
            // case rendered outside the ECS entirely.
            ImGui::SeparatorText("Models");
            int model_index = 0;
            ForEachWithEntity<Transform, ModelRenderer>(world, [&](Entity e, Transform& t, ModelRenderer& renderer) {
                ImGui::PushID(1000 + model_index); // offset so IDs never collide with the Bodies loop above
                bool is_selected = (e == selected_entity);
                ImVec4 label_color = is_selected ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) : ImVec4(1.0f, 0.55f, 0.2f, 1.0f);
                ImGui::TextColored(label_color, "%s", renderer.asset_path);
                ImGui::SameLine();
                if (ImGui::SmallButton(is_selected ? "Selected" : "Select")) {
                    selected_entity = e;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete")) {
                    delete_confirm_target = e;
                    delete_confirm_name = std::string("Model \"") + renderer.asset_path + "\"";
                }
                ImGui::DragFloat3("Position (m)", &t.position.x, 0.05f);
                ImGui::Separator();
                ImGui::PopID();
                ++model_index;
            });
            ImGui::End();

            // Shared "Are you sure?" confirmation for every Delete button
            // above (and in the Hierarchy panel below) — friendlier than
            // deleting immediately and irreversibly on a single misclick.
            // OpenPopup is safe to call every frame while the target is set:
            // ImGui no-ops it once the popup's already open.
            if (!delete_confirm_target.IsNull()) {
                ImGui::OpenPopup("Confirm Delete");
            }
            if (ImGui::BeginPopupModal("Confirm Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Delete %s?", delete_confirm_name.c_str());
                ImGui::TextDisabled("This cannot be undone.");
                ImGui::Separator();
                if (ImGui::Button("Delete", ImVec2(120, 0))) {
                    entities_to_delete.push_back(delete_confirm_target);
                    delete_confirm_target = kNullEntity;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    delete_confirm_target = kNullEntity;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // Asset browser follow-up: what used to be an inline "Add Model"
            // button list inside the main panel is now its own dedicated
            // window, with a Refresh button re-running ListAvailableGltfModels()
            // so a .gltf dropped into assets/models/ while the editor is
            // running shows up without a restart.
            if (show_asset_browser) {
                ImGui::SetNextWindowPos(ImVec2(kPanelMargin, kMenuBarHeight + kPanelMargin + 340.0f + kPanelMargin),
                                         ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(kLeftPanelWidth, 220.0f), ImGuiCond_FirstUseEver);
                ImGui::Begin("Asset Browser", &show_asset_browser);
                ImGui::TextWrapped("Click Spawn to add a model to the scene.");
                ImGui::Text("assets/models/*.gltf");
                ImGui::SameLine();
                if (ImGui::SmallButton("Refresh")) {
                    available_gltf_models = ListAvailableGltfModels();
                }
                ImGui::Separator();
                if (available_gltf_models.empty()) {
                    ImGui::TextDisabled("(no .gltf files found under assets/models/)");
                }
                for (const std::string& model_path : available_gltf_models) {
                    ImGui::PushID(model_path.c_str());
                    ImGui::Text("%s", model_path.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Spawn")) {
                        Vec3 spawn_pos(spread(rng), 1.2f, spread(rng) + 3.5f);
                        Entity new_entity =
                            world.CreateEntity(Transform{spawn_pos, Quaternion::Identity()}, ModelRenderer{});
                        SetModelPath(*world.GetComponent<ModelRenderer>(new_entity), model_path);
                        commands.Record(editor::CreateEntityCommand::FromExisting(cmd_ctx, new_entity, "Spawn Model"));
                    }
                    ImGui::PopID();
                }
                ImGui::End();
            }

            // Scene-hierarchy follow-up: a real parent/child tree view,
            // replacing the flat Bodies/Models lists' implicit assumption
            // that every entity is independent. "Parent to Selection" on a
            // row nests that row's entity under whatever's currently
            // selected (rejecting the drop if it would create a cycle — see
            // WouldCreateCycle); "Unparent" detaches it back to the root.
            if (show_hierarchy) {
            ImGui::SetNextWindowPos(
                ImVec2(kPanelMargin, kMenuBarHeight + kPanelMargin + 340.0f + kPanelMargin + 220.0f + kPanelMargin),
                ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(kLeftPanelWidth, 220.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Hierarchy", &show_hierarchy);
            ImGui::TextWrapped(
                "Select an entity, then click \"Parent to selection\" on another row to nest it underneath.");
            ImGui::Separator();

            std::vector<Entity> all_entities;
            ForEachWithEntity<Transform>(world, [&](Entity e, Transform&) { all_entities.push_back(e); });

            std::unordered_map<u32, std::vector<Entity>> children_of; // keyed by Parent entity's index
            std::vector<Entity> roots;
            for (Entity e : all_entities) {
                Parent* p = world.GetComponent<Parent>(e);
                if (p && !p->entity.IsNull()) {
                    children_of[p->entity.index].push_back(e);
                } else {
                    roots.push_back(e);
                }
            }

            auto entity_label = [&](Entity e) -> std::string {
                if (ModelRenderer* mr = world.GetComponent<ModelRenderer>(e)) {
                    return std::string("Model: ") + mr->asset_path;
                }
                if (world.HasComponent<RigidBody>(e)) {
                    return "Body #" + std::to_string(e.index);
                }
                return "Entity #" + std::to_string(e.index);
            };

            std::function<void(Entity)> draw_hierarchy_node = [&](Entity e) {
                ImGui::PushID(static_cast<int>(e.index));
                auto it = children_of.find(e.index);
                bool has_children = it != children_of.end() && !it->second.empty();
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (!has_children) {
                    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                }
                if (e == selected_entity) {
                    flags |= ImGuiTreeNodeFlags_Selected;
                }
                bool open = ImGui::TreeNodeEx(entity_label(e).c_str(), flags);
                if (ImGui::IsItemClicked()) {
                    selected_entity = e;
                }
                ImGui::SameLine();
                if (!selected_entity.IsNull() && selected_entity != e &&
                    !WouldCreateCycle(world, e, selected_entity)) {
                    if (ImGui::SmallButton("Parent to selection")) {
                        world.AddComponent(e, Parent{selected_entity});
                    }
                    ImGui::SameLine();
                }
                if (world.HasComponent<Parent>(e)) {
                    if (ImGui::SmallButton("Unparent")) {
                        world.RemoveComponent<Parent>(e);
                    }
                    ImGui::SameLine();
                }
                if (ImGui::SmallButton("Delete")) {
                    delete_confirm_target = e;
                    delete_confirm_name = entity_label(e);
                }
                if (has_children && open) {
                    for (Entity child : it->second) {
                        draw_hierarchy_node(child);
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            };

            for (Entity root : roots) {
                draw_hierarchy_node(root);
            }
            ImGui::End();
            } // show_hierarchy

            // Inspector: shows only the selected entity, regardless of which
            // list it was selected from — the "click-to-select" workflow's
            // payoff panel. Selection itself is still list-based (a "Select"
            // button per row above), not a true click-in-viewport pick —
            // that needs screen-to-world ray casting against each entity's
            // bounds, real future work.
            if (show_inspector && !selected_entity.IsNull()) {
                ImGui::SetNextWindowPos(
                    ImVec2(static_cast<f32>(swap_chain.Width()) - 320.0f - kPanelMargin, kMenuBarHeight + kPanelMargin),
                    ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(320.0f, 300.0f), ImGuiCond_FirstUseEver);
                ImGui::Begin("Inspector", &show_inspector);
                // Every reflected component on the entity is drawn
                // generically from its reflection data, so a newly reflected
                // component shows up here with no editor code.
                // Every edit, add and remove is an undoable command; the
                // physics side effects live in `hooks` above, so they also
                // happen on undo and redo.
                editor::InspectEntity(cmd_ctx, commands, selected_entity);

                // glTF runtime details that aren't component data (load
                // status, animation playback) stay hand-drawn below.
                if (world.HasComponent<ModelRenderer>(selected_entity)) {
                    ModelRenderer& renderer = *world.GetComponent<ModelRenderer>(selected_entity);
                    ImGui::SeparatorText("glTF Model");
                    GltfRenderData& data = GetOrLoadGltfRenderData(device, gltf_asset_manager, gltf_cache,
                                                                    renderer.asset_path);
                    if (!data.valid) {
                        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Failed to load — see the log");
                    } else {
                        ImGui::Text("%zu index(es), material baseColor=(%.2f,%.2f,%.2f)", data.index_count,
                                    data.material.base_color[0], data.material.base_color[1],
                                    data.material.base_color[2]);
                        if (!data.scene.animations.empty()) {
                            ImGui::Checkbox("Animate", &data.animate);
                            ImGui::Text("Time: %.2f / %.2fs", data.anim_time, data.scene.animations[0].duration);
                        } else {
                            ImGui::Text("(no animations in this asset)");
                        }
                    }
                }
                if (ImGui::Button("Deselect")) {
                    selected_entity = kNullEntity;
                }
                ImGui::End();
            }

            // Onboarding follow-up: shown by default on first run so a new
            // user isn't left guessing what the mouse/keys do — the single
            // biggest gap the earlier gizmo/picking/camera follow-up left
            // completely undocumented in-app. The title-bar close (X), wired
            // to show_help, is how it gets dismissed; "Help > Show Help
            // Window" in the menu bar brings it back.
            if (show_help) {
                ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver,
                                         ImVec2(0.5f, 0.5f));
                ImGui::Begin("Help", &show_help);
                ImGui::TextWrapped("Welcome to the Aether Editor. Quick reference:");
                ImGui::SeparatorText("Camera");
                ImGui::BulletText("Hold Right Mouse Button + move to look around.");
                ImGui::BulletText("W/A/S/D to move, E/Q to rise/fall (while holding RMB).");
                ImGui::SeparatorText("Selecting & moving");
                ImGui::BulletText("Left-click an object in the 3D view to select it.");
                ImGui::BulletText("Left-click empty space to deselect.");
                ImGui::BulletText("Drag a red/green/blue gizmo arrow to move the selection along that axis.");
                ImGui::BulletText("Or use the Position fields in the Bodies/Models/Inspector panels.");
                ImGui::SeparatorText("Scene");
                ImGui::BulletText("The Hierarchy panel shows parent/child relationships.");
                ImGui::BulletText("\"Parent to selection\" nests an entity under whatever's selected.");
                ImGui::BulletText("The Asset Browser spawns new models from assets/models/*.gltf.");
                ImGui::Separator();
                if (ImGui::Button("Got it", ImVec2(120, 0))) {
                    show_help = false;
                }
                ImGui::End();
            }

            ImGui::Render();

            for (Entity e : entities_to_delete) {
                if (!world.IsAlive(e)) {
                    continue; // e.g. deleted twice in one frame
                }
                // Unparent anything that pointed at this entity before
                // destroying it — a Parent left dangling at a stale/reused
                // index would corrupt ComputeWorldTransform and the
                // Hierarchy panel's tree for the next entity that happens to
                // land in that slot. Collected first, applied after: calling
                // RemoveComponent (which migrates the entity to a different
                // archetype) from inside ForEachWithEntity's own archetype
                // iteration would invalidate that iteration mid-flight.
                std::vector<Entity> orphaned_children;
                ForEachWithEntity<Parent>(world, [&](Entity child, Parent& p) {
                    if (p.entity == e) {
                        orphaned_children.push_back(child);
                    }
                });
                for (Entity child : orphaned_children) {
                    world.RemoveComponent<Parent>(child);
                }
                if (e == selected_entity) {
                    selected_entity = kNullEntity;
                }
                // Undoable; the Jolt body goes via hooks.on_entity_destroying.
                // (Unparenting the children above isn't recorded, so undo
                // restores the entity without re-linking them.)
                EntityGuid guid = EnsureGuid(world, e, &guids);
                commands.Execute(cmd_ctx, std::make_unique<editor::DestroyEntityCommand>(guid));
            }

            // Advance each cached asset's animation once per frame (not once
            // per entity referencing it — see GltfRenderData's comment on
            // why that's shared, not per-instance).
            for (auto& [path, data] : gltf_cache) {
                if (!data->valid || data->scene.animations.empty()) {
                    continue;
                }
                const assets::GltfAnimation& animation = data->scene.animations[0];
                if (data->animate) {
                    data->anim_time += kDt;
                    if (animation.duration > 0.0f) {
                        data->anim_time = std::fmod(data->anim_time, animation.duration);
                    }
                }
                assets::EvaluateAnimation(data->scene, animation, data->anim_time, data->scene.node_instances);
            }

            // view/proj/view_proj were already computed at the top of this
            // iteration (right after NewFrame()) so picking/gizmo math could
            // use them too — no longer recomputed here.
            view_proj_buffer.Update(&view_proj, sizeof(f32) * 16);
            if (!instances.empty()) {
                instance_buffer.Update(instances.data(), sizeof(EditorInstance) * instances.size());
            }

            // Gizmo geometry: 3 axis lines (X=red, Y=green, Z=blue) rebuilt
            // every frame at the selected entity's current position — cheap
            // enough (6 vertices) that a dynamic per-frame upload is simpler
            // than tracking whether it actually moved.
            bool draw_gizmo = false;
            GizmoVertex gizmo_vertices[6]{};
            if (!selected_entity.IsNull()) {
                Transform* sel_t = world.GetComponent<Transform>(selected_entity);
                if (sel_t) {
                    Vec3 origin = world.HasComponent<ModelRenderer>(selected_entity) ? WorldPosition(world, selected_entity)
                                                                                      : sel_t->position;
                    const Vec3 axes[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
                    const f32 axis_colors[3][3] = {{1, 0.2f, 0.2f}, {0.2f, 1, 0.2f}, {0.3f, 0.5f, 1}};
                    for (int a = 0; a < 3; ++a) {
                        Vec3 tip = origin + axes[a] * kGizmoAxisLength;
                        gizmo_vertices[a * 2 + 0] = {{origin.x, origin.y, origin.z},
                                                      {axis_colors[a][0], axis_colors[a][1], axis_colors[a][2]}};
                        gizmo_vertices[a * 2 + 1] = {{tip.x, tip.y, tip.z},
                                                      {axis_colors[a][0], axis_colors[a][1], axis_colors[a][2]}};
                    }
                    gizmo_vertex_buffer.Update(gizmo_vertices, sizeof(gizmo_vertices));
                    draw_gizmo = true;
                }
            }

            u32 buffer_index = swap_chain.CurrentBackBufferIndex();
            last_rendered_buffer_index = buffer_index;
            device.WaitForFence(frame_fences[buffer_index]);

            CommandList& cmd = *command_lists[buffer_index];
            cmd.Reset();

            ID3D12Resource* back_buffer = swap_chain.CurrentBackBuffer();
            RenderGraph::ResourceHandle backbuffer_handle =
                graph.ImportResource(back_buffer, D3D12_RESOURCE_STATE_PRESENT, "BackBuffer");

            graph.AddPass(
                "EditorForward",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Write(backbuffer_handle, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    builder.Write(depth_handle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                },
                [&](ID3D12GraphicsCommandList* cl) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swap_chain.CurrentBackBufferRTV();
                    D3D12_CPU_DESCRIPTOR_HANDLE dsv = graph.GetOrCreateDSV(depth_handle);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                    const f32 clear_color[4] = {0.03f, 0.03f, 0.05f, 1.0f};
                    cl->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
                    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<f32>(swap_chain.Width()),
                                             static_cast<f32>(swap_chain.Height()), 0.0f, 1.0f};
                    D3D12_RECT scissor{0, 0, static_cast<LONG>(swap_chain.Width()),
                                        static_cast<LONG>(swap_chain.Height())};
                    cl->RSSetViewports(1, &viewport);
                    cl->RSSetScissorRects(1, &scissor);

                    if (!instances.empty()) {
                        cl->SetPipelineState(pso.Get());
                        cl->SetGraphicsRootSignature(root_signature.Get());
                        cl->SetGraphicsRootConstantBufferView(0, view_proj_buffer.GPUAddress());
                        cl->SetGraphicsRootShaderResourceView(1, instance_buffer.GPUAddress());
                        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        cl->DrawInstanced(6, static_cast<UINT>(instances.size()), 0, 0);
                    }

                    // Every (Transform, ModelRenderer) entity draws with its
                    // own Transform as the model's world placement — no more
                    // single hardcoded offset, since a model is now a normal
                    // entity that can be spawned/moved/deleted like any
                    // other.
                    bool any_model_bound = false;
                    ForEachWithEntity<Transform, ModelRenderer>(
                        world, [&](Entity e, Transform&, ModelRenderer& renderer) {
                            auto it = gltf_cache.find(renderer.asset_path);
                            if (it == gltf_cache.end() || !it->second->valid) {
                                return;
                            }
                            GltfRenderData& data = *it->second;

                            if (!any_model_bound) {
                                cl->SetPipelineState(gltf_pso.Get());
                                cl->SetGraphicsRootSignature(gltf_root_signature.Get());
                                ID3D12DescriptorHeap* gltf_heaps[] = {gltf_bindless_heap.Heap()};
                                cl->SetDescriptorHeaps(1, gltf_heaps);
                                cl->SetGraphicsRootDescriptorTable(2, gltf_bindless_heap.GPUHandle(0));
                                cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                                any_model_bound = true;
                            }
                            cl->IASetVertexBuffers(0, 1, &data.vbv);
                            cl->IASetIndexBuffer(&data.ibv);
                            cl->SetGraphicsRoot32BitConstants(1, sizeof(MaterialData) / 4, &data.material, 0);

                            // Scene-hierarchy follow-up: composed from the
                            // Parent chain (identity for an entity with no
                            // Parent), not just this entity's own local
                            // Transform — see ComputeWorldTransform's
                            // comment.
                            Mat4 entity_transform = ComputeWorldTransform(world, e);
                            f32 highlight = (e == selected_entity) ? 1.0f : 0.0f;
                            for (const assets::GltfNodeInstance& node_instance : data.scene.node_instances) {
                                Mat4 model = entity_transform * node_instance.world_transform;
                                GltfFrameConstants gltf_frame_constants{};
                                gltf_frame_constants.model = model;
                                gltf_frame_constants.mvp = view_proj * model;
                                gltf_frame_constants.camera_pos = camera.position;
                                gltf_frame_constants.light_dir = Vec3(-0.4f, -0.8f, -0.3f).Normalized();
                                gltf_frame_constants.highlight = highlight;
                                cl->SetGraphicsRoot32BitConstants(0, sizeof(GltfFrameConstants) / 4,
                                                                   &gltf_frame_constants, 0);
                                cl->DrawIndexedInstanced(static_cast<UINT>(data.index_count), 1, 0, 0, 0);
                            }
                        });

                    // Transform-gizmo follow-up: 3 draggable axis lines for
                    // whatever's currently selected, drawn last (and with
                    // depth testing off, see CreateGizmoPSO) so they're
                    // always visible on top of the scene.
                    if (draw_gizmo) {
                        D3D12_VERTEX_BUFFER_VIEW gizmo_vbv{};
                        gizmo_vbv.BufferLocation = gizmo_vertex_buffer.GPUAddress();
                        gizmo_vbv.SizeInBytes = static_cast<UINT>(gizmo_vertex_buffer.Size());
                        gizmo_vbv.StrideInBytes = sizeof(GizmoVertex);

                        cl->SetPipelineState(gizmo_pso.Get());
                        cl->SetGraphicsRootSignature(gizmo_root_signature.Get());
                        cl->SetGraphicsRootConstantBufferView(0, view_proj_buffer.GPUAddress());
                        cl->IASetVertexBuffers(0, 1, &gizmo_vbv);
                        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
                        cl->DrawInstanced(6, 1, 0, 0);
                    }

                    ID3D12DescriptorHeap* imgui_heaps[] = {imgui_srv_heap.Heap()};
                    cl->SetDescriptorHeaps(1, imgui_heaps);
                    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
                });

            graph.Execute(cmd.Get());
            graph.Reset();

            cmd.Close();
            ID3D12CommandList* lists[] = {cmd.Get()};
            frame_fences[buffer_index] = device.Submit(lists, 1);

            swap_chain.Present(/*vsync=*/true);

            ++frame_index;
            if (max_frames >= 0 && frame_index >= max_frames) {
                AETHER_LOG_INFO("Editor", "Reached AETHER_EDITOR_MAX_FRAMES=%d, exiting", max_frames);
                break;
            }
        }

        for (u64 fence : frame_fences) {
            device.WaitForFence(fence);
        }

        if (screenshot_path) {
            SaveBackbufferScreenshot(device, swap_chain, last_rendered_buffer_index, screenshot_path);
        }

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        AETHER_LOG_INFO("Editor", "Shutting down cleanly");
    } catch (const std::exception& e) {
        AETHER_LOG_FATAL("Editor", "Unhandled exception: %s", e.what());
        return 1;
    }

    return 0;
}
