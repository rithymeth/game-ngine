#pragma once

#include "aether/gfx/buffer.h"
#include "aether/gfx/device.h"
#include "aether/math/vec.h"
#include "aether/core/base.h"

#include <d3d12.h>
#include <vector>

// Viewport debug drawing (Phase 23 step 6): expands the accumulated
// dev::DebugPrimitive queue (snapshot of DebugDrawInstance) into a colored
// line-list the editor renders with the gizmo-style PSO. Boxes, spheres,
// points and rects are tessellated into wireframe segments on the CPU once
// per frame; text is drawn as a small cross marker (ImGui rasterizes the
// actual text in the overlay).
//
// Mirrors the gizmo's GizmoVertex layout (pos + color) so it can share a
// root signature and PSO: two D3D12_INPUT_ELEMENT_DESCs (R32G32B32_FLOAT
// position, R32G32B32_FLOAT color), a CBV at register b0 for the view/proj.
// The line-list PSO in main.cpp (CreateGizmoPSO) is used directly.

namespace aether::editor {

struct DebugDrawVertex {
    f32 pos[3];
    f32 color[3];
};

constexpr usize kMaxDebugDrawVerts = 65536; // 32k segments; plenty for a frame of debug lines

// Accumulates vertex data for one frame of debug primitives.
class DebugDrawBuilder {
public:
    DebugDrawVertex* Next(usize count) {
        usize base = verts_.size();
        if (base + count > kMaxDebugDrawVerts) return nullptr;
        verts_.resize(base + count);
        return verts_.data() + base;
    }

    void AddLine(const Vec3& a, const Vec3& b, u32 color) {
        if (DebugDrawVertex* v = Next(2)) {
            const Vec3 ca = ColorOf(color);
            v[0] = {{a.x, a.y, a.z}, {ca.x, ca.y, ca.z}};
            v[1] = {{b.x, b.y, b.z}, {ca.x, ca.y, ca.z}};
        }
    }

    // Adds axis-aligned box wireframe (12 edges) in world space.
    void AddBox(const Vec3& min, const Vec3& max, u32 color) {
        const Vec3 c = ColorOf(color);
        if (DebugDrawVertex* v = Next(24)) {
            const f32 mn[3] = {min.x, min.y, min.z};
            const f32 mx[3] = {max.x, max.y, max.z};
            // The 12 edges of a box.
            const int edges[24][2] = {{0,1},{0,2},{0,4},{1,3},{1,5},{2,3},{2,6},{3,7},{4,5},{4,6},{5,7},{6,7}};
            const f32 corners[8][3] = {
                {mn[0], mn[1], mn[2]}, {mx[0], mn[1], mn[2]}, {mn[0], mx[1], mn[2]}, {mx[0], mx[1], mn[2]},
                {mn[0], mn[1], mx[2]}, {mx[0], mn[1], mx[2]}, {mn[0], mx[1], mx[2]}, {mx[0], mx[1], mx[2]}};
            for (int e = 0; e < 12; ++e) {
                const float* p0 = corners[edges[e][0]];
                const float* p1 = corners[edges[e][1]];
                v[e * 2 + 0] = {{p0[0], p0[1], p0[2]}, {c.x, c.y, c.z}};
                v[e * 2 + 1] = {{p1[0], p1[1], p1[2]}, {c.x, c.y, c.z}};
            }
        }
    }

    // Adds a wireframe sphere (3 rings of `segs` segments each).
    void AddSphere(const Vec3& center, f32 radius, u32 color, int segs = 16) {
        const Vec3 c = ColorOf(color);
        for (int ring = 0; ring < 3; ++ring) {
            if (DebugDrawVertex* v = Next(static_cast<usize>(segs) * 2)) {
                for (int i = 0; i < segs; ++i) {
                    const float t0 = 2.0f * 3.14159265f * static_cast<float>(i) / static_cast<float>(segs);
                    const float t1 = 2.0f * 3.14159265f * static_cast<float>(i + 1) / static_cast<float>(segs);
                    Vec3 p0, p1;
                    if (ring == 0) { p0 = center + Vec3(std::cos(t0) * radius, 0, std::sin(t0) * radius); p1 = center + Vec3(std::cos(t1) * radius, 0, std::sin(t1) * radius); }
                    else if (ring == 1) { p0 = center + Vec3(std::cos(t0) * radius, std::sin(t0) * radius, 0); p1 = center + Vec3(std::cos(t1) * radius, std::sin(t1) * radius, 0); }
                    else { p0 = center + Vec3(0, std::cos(t0) * radius, std::sin(t0) * radius); p1 = center + Vec3(0, std::cos(t1) * radius, std::sin(t1) * radius); }
                    v[i * 2 + 0] = {{p0.x, p0.y, p0.z}, {c.x, c.y, c.z}};
                    v[i * 2 + 1] = {{p1.x, p1.y, p1.z}, {c.x, c.y, c.z}};
                }
            }
        }
    }

    // Adds a small 3-axis cross (a point marker).
    void AddPoint(const Vec3& center, f32 half_size, u32 color) {
        const Vec3 c = ColorOf(color);
        const f32 h = half_size;
        const Vec3 axes[3] = {Vec3(h, 0, 0), Vec3(0, h, 0), Vec3(0, 0, h)};
        for (int i = 0; i < 3; ++i) {
            if (DebugDrawVertex* v = Next(2)) {
                const Vec3 a = center - axes[i], b = center + axes[i];
                v[0] = {{a.x, a.y, a.z}, {c.x, c.y, c.z}};
                v[1] = {{b.x, b.y, b.z}, {c.x, c.y, c.z}};
            }
        }
    }

    // A flat rect: two triangles' edges (a debug box flattened along one axis).
    void AddRect(const Vec3& center, const Vec3& size, u32 color) {
        const Vec3 c = ColorOf(color);
        const Vec3 mn = center - size, mx = center + size;
        const Vec3 corners[4] = {Vec3(mn.x, mn.y, mn.z), Vec3(mx.x, mn.y, mn.z), Vec3(mx.x, mx.y, mx.z), Vec3(mn.x, mx.y, mx.z)};
        if (DebugDrawVertex* v = Next(8)) {
            for (int i = 0; i < 4; ++i) {
                const Vec3 a = corners[i], b = corners[(i + 1) % 4];
                v[i * 2 + 0] = {{a.x, a.y, a.z}, {c.x, c.y, c.z}};
                v[i * 2 + 1] = {{b.x, b.y, b.z}, {c.x, c.y, c.z}};
            }
        }
    }

    void Clear() { verts_.clear(); }
    bool Empty() const { return verts_.empty(); }
    usize VertexCount() const { return verts_.size(); }
    const DebugDrawVertex* Data() const { return verts_.data(); }

    // 0xRRGGBBAA -> float RGB.
    static Vec3 ColorOf(u32 color) {
        const float r = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
        const float g = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
        const float b = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
        return {r, g, b};
    }

private:
    std::vector<DebugDrawVertex> verts_;
};

} // namespace aether::editor
