// Abyss Protocol viewer (Windows).
//
// Software 3D rasteriser blitted with GDI, same idea as Gem Hop, so a loaded
// glTF draws without a GPU. Gameplay stays in abyss_protocol.cpp.
//
//   W/A/S/D   thrust (camera-relative)     Shift   quiet thrust
//   Q/E       dive / rise                  F       sonar ping
//   Mouse or arrows   look                 L       toggle lights
//   R         restart                      Esc     quit
//
//   abyss_protocol.exe path\to\model.glb
//   AETHER_ABYSS_MODEL, AETHER_ABYSS_MAX_FRAMES, AETHER_ABYSS_SCREENSHOT

#include "abyss_protocol.h"

#include "aether/assets/gltf_loader.h"
#include "aether/core/log.h"
#include "aether/math/mat4.h"
#include "aether/platform/window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::assets;

namespace {

constexpr i32 kW = 960;
constexpr i32 kH = 540;
constexpr int kMaxTris = 4000;

struct Color {
    f32 r, g, b;
};

struct DrawTri {
    Vec3 a, b, c;
    Color col;
};

u32 Rgb(f32 r, f32 g, f32 b) {
    auto c = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    return (c(r) << 16) | (c(g) << 8) | c(b);
}

struct Camera {
    Vec3 pos;
    Vec3 right, up, fwd;
    f32 focal = 0;

    void LookAt(const Vec3& eye, const Vec3& target) {
        pos = eye;
        fwd = (target - eye).Normalized();
        right = fwd.Cross({0, 1, 0}).Normalized();
        up = right.Cross(fwd);
        focal = (kH * 0.5f) / std::tan(0.5f * 1.05f);
    }
    Vec3 ToView(const Vec3& w) const {
        Vec3 d = w - pos;
        return {d.Dot(right), d.Dot(up), d.Dot(fwd)};
    }
};

class Renderer {
public:
    Renderer() : color_(static_cast<usize>(kW) * kH), depth_(static_cast<usize>(kW) * kH) {}

    const std::vector<u32>& Pixels() const { return color_; }
    void SetAmbient(f32 ambient) { ambient_ = ambient; }

    void Clear(Color top, Color bottom) {
        for (i32 y = 0; y < kH; ++y) {
            f32 t = static_cast<f32>(y) / kH;
            u32 sky = Rgb(top.r + (bottom.r - top.r) * t, top.g + (bottom.g - top.g) * t,
                          top.b + (bottom.b - top.b) * t);
            std::fill_n(color_.begin() + static_cast<usize>(y) * kW, kW, sky);
        }
        std::fill(depth_.begin(), depth_.end(), 0.0f);
    }

    void BeginFrame(const Camera& cam) { cam_ = cam; }

    void Tri(const Vec3& a, const Vec3& b, const Vec3& c, Color col) {
        Vec3 n = (b - a).Cross(c - a);
        f32 len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len < 1e-6f) return;
        n = n * (1.0f / len);
        static const Vec3 light = Vec3{0.2f, 0.85f, 0.25f}.Normalized();
        f32 shade = ambient_ + (1.0f - ambient_) * std::max(0.0f, n.Dot(light));
        RasterTri(a, b, c, Rgb(col.r * shade, col.g * shade, col.b * shade));
    }

    void Quad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, Color col) {
        Tri(a, b, c, col);
        Tri(a, c, d, col);
    }

    void Box(const Vec3& lo, const Vec3& hi, Color col) {
        Vec3 p[8];
        for (int i = 0; i < 8; ++i) {
            p[i] = {(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
        }
        Quad(p[2], p[6], p[7], p[3], col);
        Quad(p[0], p[1], p[5], p[4], col);
        Quad(p[1], p[3], p[7], p[5], col);
        Quad(p[0], p[4], p[6], p[2], col);
        Quad(p[4], p[5], p[7], p[6], col);
        Quad(p[0], p[2], p[3], p[1], col);
    }

    void Gem(const Vec3& c, f32 r, f32 spin, Color col) {
        f32 s = std::sin(spin), co = std::cos(spin);
        auto rot = [&](f32 x, f32 y, f32 z) { return Vec3{c.x + x * co - z * s, c.y + y, c.z + x * s + z * co}; };
        Vec3 top = rot(0, r * 1.4f, 0), bot = rot(0, -r, 0);
        Vec3 e[4] = {rot(r, 0, 0), rot(0, 0, r), rot(-r, 0, 0), rot(0, 0, -r)};
        for (int i = 0; i < 4; ++i) {
            const Vec3& a = e[i];
            const Vec3& b = e[(i + 1) % 4];
            Tri(top, a, b, col);
            Tri(bot, b, a, col);
        }
    }

private:
    void RasterTri(const Vec3& wa, const Vec3& wb, const Vec3& wc, u32 rgb) {
        constexpr f32 kNear = 0.15f;
        Vec3 in[3] = {cam_.ToView(wa), cam_.ToView(wb), cam_.ToView(wc)};
        Vec3 poly[4];
        int count = 0;
        for (int i = 0; i < 3; ++i) {
            const Vec3& p = in[i];
            const Vec3& q = in[(i + 1) % 3];
            bool pin = p.z >= kNear;
            bool qin = q.z >= kNear;
            if (pin) poly[count++] = p;
            if (pin != qin) {
                f32 t = (kNear - p.z) / (q.z - p.z);
                poly[count++] = p + (q - p) * t;
            }
        }
        for (int i = 1; i + 1 < count; ++i) RasterView(poly[0], poly[i], poly[i + 1], rgb);
    }

    void RasterView(const Vec3& a, const Vec3& b, const Vec3& c, u32 rgb) {
        auto proj = [&](const Vec3& v) {
            return Vec3{kW * 0.5f + cam_.focal * v.x / v.z, kH * 0.5f - cam_.focal * v.y / v.z, v.z};
        };
        Vec3 pa = proj(a), pb = proj(b), pc = proj(c);
        f32 area = (pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x);
        if (std::fabs(area) < 1e-6f) return;
        i32 x0 = std::max(0, static_cast<i32>(std::floor(std::min({pa.x, pb.x, pc.x}))));
        i32 x1 = std::min(kW - 1, static_cast<i32>(std::ceil(std::max({pa.x, pb.x, pc.x}))));
        i32 y0 = std::max(0, static_cast<i32>(std::floor(std::min({pa.y, pb.y, pc.y}))));
        i32 y1 = std::min(kH - 1, static_cast<i32>(std::ceil(std::max({pa.y, pb.y, pc.y}))));
        f32 ia = 1.0f / pa.z, ib = 1.0f / pb.z, ic = 1.0f / pc.z;
        f32 inv_area = 1.0f / area;
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                f32 px = x + 0.5f, py = y + 0.5f;
                f32 w0 = ((pb.x - px) * (pc.y - py) - (pb.y - py) * (pc.x - px)) * inv_area;
                f32 w1 = ((pc.x - px) * (pa.y - py) - (pc.y - py) * (pa.x - px)) * inv_area;
                f32 w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                f32 inv_z = w0 * ia + w1 * ib + w2 * ic;
                usize idx = static_cast<usize>(y) * kW + x;
                if (inv_z > depth_[idx]) {
                    depth_[idx] = inv_z;
                    color_[idx] = rgb;
                }
            }
        }
    }

    Camera cam_;
    f32 ambient_ = 0.35f;
    std::vector<u32> color_;
    std::vector<f32> depth_;
};

Vec3 TransformPoint(const Mat4& m, const f32 p[3]) {
    Vec4 r = m * Vec4(p[0], p[1], p[2], 1.0f);
    return {r.x, r.y, r.z};
}

void EmitPrimitive(const GltfScene& scene, const GltfPrimitive& prim, const Mat4& world, int stride, int& seen,
                   std::vector<DrawTri>& out) {
    Color col{0.92f, 0.48f, 0.16f};
    if (prim.material_index >= 0 && static_cast<usize>(prim.material_index) < scene.materials.size()) {
        const GltfMaterial& mat = scene.materials[static_cast<usize>(prim.material_index)];
        col = {mat.base_color[0], mat.base_color[1], mat.base_color[2]};
    }
    auto vert = [&](u32 index) {
        return TransformPoint(world, prim.vertices[index].position);
    };
    auto push = [&](u32 i0, u32 i1, u32 i2) {
        if (i0 >= prim.vertices.size() || i1 >= prim.vertices.size() || i2 >= prim.vertices.size()) return;
        if ((seen++ % stride) != 0) return;
        if (static_cast<int>(out.size()) >= kMaxTris) return;
        out.push_back({vert(i0), vert(i1), vert(i2), col});
    };
    if (!prim.indices.empty()) {
        for (usize i = 0; i + 2 < prim.indices.size(); i += 3) push(prim.indices[i], prim.indices[i + 1], prim.indices[i + 2]);
    } else {
        for (usize i = 0; i + 2 < prim.vertices.size(); i += 3)
            push(static_cast<u32>(i), static_cast<u32>(i + 1), static_cast<u32>(i + 2));
    }
}

int CountPrimitives(const GltfScene& scene, const GltfPrimitive& prim) {
    if (!prim.indices.empty()) return static_cast<int>(prim.indices.size() / 3);
    return static_cast<int>(prim.vertices.size() / 3);
}

std::vector<DrawTri> BuildMesh(const GltfScene& scene) {
    int total = 0;
    auto visit = [&](auto&& consume) {
        if (!scene.node_instances.empty()) {
            for (const GltfNodeInstance& inst : scene.node_instances) {
                if (inst.mesh_index >= scene.meshes.size()) continue;
                for (const GltfPrimitive& prim : scene.meshes[inst.mesh_index].primitives) consume(prim, inst.world_transform);
            }
        } else {
            for (const GltfMesh& mesh : scene.meshes) {
                for (const GltfPrimitive& prim : mesh.primitives) consume(prim, Mat4::Identity());
            }
        }
    };
    visit([&](const GltfPrimitive& prim, const Mat4&) { total += CountPrimitives(scene, prim); });
    int stride = std::max(1, total / kMaxTris);

    std::vector<DrawTri> tris;
    int seen = 0;
    visit([&](const GltfPrimitive& prim, const Mat4& world) { EmitPrimitive(scene, prim, world, stride, seen, tris); });
    if (tris.empty()) return tris;

    Vec3 lo = tris[0].a, hi = tris[0].a;
    auto grow = [&](const Vec3& p) {
        lo.x = std::min(lo.x, p.x);
        lo.y = std::min(lo.y, p.y);
        lo.z = std::min(lo.z, p.z);
        hi.x = std::max(hi.x, p.x);
        hi.y = std::max(hi.y, p.y);
        hi.z = std::max(hi.z, p.z);
    };
    for (const DrawTri& t : tris) {
        grow(t.a);
        grow(t.b);
        grow(t.c);
    }
    Vec3 center{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    f32 extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-4f});
    f32 scale = 4.0f / extent;
    for (DrawTri& t : tris) {
        auto fit = [&](Vec3 p) { return Vec3{(p.x - center.x) * scale, (p.y - center.y) * scale, (p.z - center.z) * scale}; };
        t.a = fit(t.a);
        t.b = fit(t.b);
        t.c = fit(t.c);
    }
    return tris;
}

void FillRect(std::vector<u32>& px, i32 x, i32 y, i32 w, i32 h, u32 c) {
    for (i32 yy = std::max(0, y); yy < std::min(kH, y + h); ++yy) {
        for (i32 xx = std::max(0, x); xx < std::min(kW, x + w); ++xx) px[static_cast<usize>(yy) * kW + xx] = c;
    }
}

void DrawBar(std::vector<u32>& px, i32 x, i32 y, f32 amount, u32 fill) {
    FillRect(px, x, y, 160, 10, Rgb(0.08f, 0.1f, 0.12f));
    FillRect(px, x, y, static_cast<i32>(160 * std::clamp(amount, 0.0f, 1.0f)), 10, fill);
}

bool Near(const Vec3& a, const Vec3& b, f32 range) {
    f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz <= range * range;
}

// Lights show what is close. A sonar ping shows farther, for a few seconds.
bool Visible(const abyss::Game& game, const Vec3& p, f32 light_range, f32 sonar_range, f32 always) {
    const Vec3& sub = game.GetSub().position;
    if (Near(sub, p, always)) return true;
    if (game.GetSub().lights_on && Near(sub, p, light_range)) return true;
    if (game.HasContact() && Near(sub, p, sonar_range)) return true;
    return false;
}

void DrawWorld(const abyss::Game& game, const std::vector<DrawTri>& mesh, Renderer& r, f32 yaw, f32 pitch, f32 anim) {
    const abyss::Submarine& sub = game.GetSub();
    Vec3 target = sub.position;
    f32 dist = 8.0f;
    Vec3 eye = target + Vec3{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)} * dist;
    Camera cam;
    cam.LookAt(eye, target);

    if (sub.lights_on) {
        r.SetAmbient(0.42f);
        r.Clear({0.05f, 0.18f, 0.28f}, {0.01f, 0.05f, 0.1f});
    } else {
        r.SetAmbient(0.12f);
        r.Clear({0.01f, 0.03f, 0.06f}, {0.0f, 0.01f, 0.02f});
    }
    r.BeginFrame(cam);

    f32 floor_y = -42.0f;
    i32 gx = static_cast<i32>(std::floor(sub.position.x / 10.0f));
    i32 gz = static_cast<i32>(std::floor(sub.position.z / 10.0f));
    i32 span = sub.lights_on ? 2 : (game.HasContact() ? 1 : 0);
    for (i32 iz = -span; iz <= span; ++iz) {
        for (i32 ix = -span; ix <= span; ++ix) {
            f32 x = (gx + ix) * 10.0f;
            f32 z = (gz + iz) * 10.0f;
            bool alt = ((gx + ix + gz + iz) & 1) != 0;
            Color tile = sub.lights_on ? (alt ? Color{0.12f, 0.34f, 0.38f} : Color{0.08f, 0.22f, 0.28f})
                                       : (alt ? Color{0.04f, 0.10f, 0.12f} : Color{0.02f, 0.06f, 0.08f});
            r.Box({x, floor_y - 1.2f, z}, {x + 9.2f, floor_y, z + 9.2f}, tile);
        }
    }

    if (!mesh.empty()) {
        for (const DrawTri& t : mesh) {
            r.Tri(t.a + sub.position, t.b + sub.position, t.c + sub.position, t.col);
        }
    } else {
        Vec3 p = sub.position;
        r.Box({p.x - 1.1f, p.y - 0.55f, p.z - 2.2f}, {p.x + 1.1f, p.y + 0.55f, p.z + 2.2f}, {0.85f, 0.32f, 0.12f});
        r.Box({p.x - 0.5f, p.y + 0.2f, p.z + 0.3f}, {p.x + 0.5f, p.y + 1.0f, p.z + 1.6f}, {0.55f, 0.85f, 0.95f});
    }

    if (game.HasContact()) {
        f32 age = 1.0f - std::clamp(game.Contact() / 3.2f, 0.0f, 1.0f);
        f32 radius = 3.0f + age * 16.0f;
        constexpr int kSegs = 18;
        for (int i = 0; i < kSegs; ++i) {
            f32 a0 = (6.2831853f * i) / kSegs;
            f32 a1 = (6.2831853f * (i + 1)) / kSegs;
            Vec3 p0{sub.position.x + std::cos(a0) * radius, sub.position.y, sub.position.z + std::sin(a0) * radius};
            Vec3 p1{sub.position.x + std::cos(a1) * radius, sub.position.y, sub.position.z + std::sin(a1) * radius};
            r.Quad(p0, p1, p1 + Vec3{0, 0.25f, 0}, p0 + Vec3{0, 0.25f, 0}, {0.3f, 0.95f, 0.85f});
        }
    }

    for (const abyss::Rock& rock : game.GetRocks()) {
        if (!Visible(game, rock.position, 22.0f, 36.0f, 5.0f)) continue;
        r.Box({rock.position.x - rock.radius, rock.position.y - rock.radius * 0.6f, rock.position.z - rock.radius},
              {rock.position.x + rock.radius, rock.position.y + rock.radius * 0.45f, rock.position.z + rock.radius},
              {0.22f, 0.24f, 0.28f});
    }

    if (game.InCurrent()) {
        for (int i = 0; i < 5; ++i) {
            f32 slide = std::fmod(anim * 3.0f + i * 1.3f, 8.0f);
            Vec3 p{20.0f + slide * 3.0f, sub.position.y - 0.2f, -12.0f + i * 3.0f};
            r.Box({p.x, p.y, p.z}, {p.x + 1.6f, p.y + 0.15f, p.z + 0.35f}, {0.25f, 0.7f, 0.85f});
        }
    }

    const abyss::Cache& cache = game.GetCache();
    if (Visible(game, cache.position, 24.0f, 40.0f, 8.0f)) {
        Color col = cache.taken ? Color{0.2f, 0.22f, 0.24f} : Color{0.95f, 0.78f, 0.25f};
        r.Box({cache.position.x - 0.6f, cache.position.y, cache.position.z - 0.6f},
              {cache.position.x + 0.6f, cache.position.y + 1.2f, cache.position.z + 0.6f}, col);
    }

    for (const abyss::CreatureAI& creature : game.GetCreatures()) {
        if (!Visible(game, creature.position, 26.0f, 40.0f, 7.0f)) continue;
        Color creature_col = creature.is_alerted ? Color{1.0f, 0.25f, 0.2f} : Color{0.25f, 0.85f, 1.0f};
        r.Gem(creature.position, creature.is_alerted ? 1.1f : 0.8f, anim * 1.5f, creature_col);
    }

    const abyss::Haven& haven = game.GetHaven();
    bool heading_home = game.GetMission() == abyss::Mission::Return;
    if (Visible(game, haven.position, 30.0f, 50.0f, 12.0f)) {
        Color pad = heading_home ? Color{0.85f, 0.7f, 0.2f} : Color{0.12f, 0.28f, 0.36f};
        r.Box({haven.position.x - 6.0f, haven.position.y - 0.5f, haven.position.z - 6.0f},
              {haven.position.x + 6.0f, haven.position.y, haven.position.z + 6.0f}, pad);
        r.Box({haven.position.x - 0.25f, haven.position.y, haven.position.z - 0.25f},
              {haven.position.x + 0.25f, haven.position.y + 4.0f, haven.position.z + 0.25f}, {0.2f, 0.45f, 0.55f});
        r.Gem({haven.position.x, haven.position.y + 4.4f, haven.position.z}, 0.35f, anim, {0.45f, 0.9f, 1.0f});
    }

    if (game.HasContact() && (game.GetMission() == abyss::Mission::Outbound || heading_home)) {
        Vec3 goal = game.Objective();
        Vec3 delta{goal.x - sub.position.x, goal.y - sub.position.y, goal.z - sub.position.z};
        f32 len = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (len > 10.0f) {
            Vec3 tip{sub.position.x + delta.x / len * 8.0f, sub.position.y + delta.y / len * 8.0f + 1.4f,
                     sub.position.z + delta.z / len * 8.0f};
            r.Gem(tip, 0.32f, anim * 4.0f, heading_home ? Color{1.0f, 0.85f, 0.25f} : Color{0.85f, 0.95f, 1.0f});
        }
    } else if (game.HasBearing() && (game.GetMission() == abyss::Mission::Outbound || heading_home)) {
        Vec3 dir = game.BearingDir();
        Vec3 tip{sub.position.x + dir.x * 8.0f, sub.position.y + dir.y * 8.0f + 1.4f, sub.position.z + dir.z * 8.0f};
        r.Gem(tip, 0.22f, 0.0f, {0.35f, 0.4f, 0.45f});
    }

    const abyss::Relay& relay = game.GetRelay();
    if (relay.restored || Visible(game, relay.position, 28.0f, 55.0f, 8.0f)) {
        Color relay_col = relay.restored ? Color{0.35f, 1.0f, 0.75f} : Color{0.45f, 0.55f, 0.62f};
        f32 pulse = relay.restored ? (0.6f + 0.4f * std::sin(anim * 3.0f)) : 1.0f;
        Vec3 rp = relay.position;
        r.Box({rp.x - 0.4f, rp.y, rp.z - 0.4f}, {rp.x + 0.4f, rp.y + 3.2f, rp.z + 0.4f},
              {relay_col.r * pulse, relay_col.g * pulse, relay_col.b * pulse});
        r.Gem({rp.x, rp.y + 3.6f, rp.z}, 0.45f, anim, relay_col);
    }
}

void DrawHud(const abyss::Game& game, std::vector<u32>& px, bool has_mesh) {
    const abyss::Submarine& sub = game.GetSub();
    FillRect(px, 0, 0, kW, 48, Rgb(0.02f, 0.04f, 0.06f));
    DrawBar(px, 16, 6, sub.battery, Rgb(0.95f, 0.75f, 0.2f));
    DrawBar(px, 16, 18, sub.oxygen, Rgb(0.45f, 0.75f, 0.95f));
    float awareness = 0.0f;
    bool alert = false;
    for (const abyss::CreatureAI& creature : game.GetCreatures()) {
        awareness = std::max(awareness, creature.awareness);
        alert = alert || creature.is_alerted;
    }
    DrawBar(px, 16, 30, awareness, alert ? Rgb(1, 0.3f, 0.2f) : Rgb(0.3f, 0.85f, 1));
    if (game.HasContact()) FillRect(px, 200, 8, 14, 32, Rgb(0.3f, 0.95f, 0.85f));
    if (sub.lights_on) FillRect(px, 220, 8, 14, 32, Rgb(1.0f, 0.9f, 0.4f));
    if (has_mesh) FillRect(px, 240, 8, 14, 32, Rgb(0.9f, 0.45f, 0.15f));
    if (game.GetMission() == abyss::Mission::Won) FillRect(px, 0, kH / 2 - 18, kW, 36, Rgb(0.08f, 0.28f, 0.16f));
    if (game.GetMission() == abyss::Mission::Lost) FillRect(px, 0, kH / 2 - 18, kW, 36, Rgb(0.32f, 0.06f, 0.06f));
}

void SaveBmp(const char* path, const std::vector<u32>& px) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = kW;
    ih.biHeight = -kH;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(px.size() * 4);
    std::FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f) {
        std::fwrite(&fh, sizeof(fh), 1, f);
        std::fwrite(&ih, sizeof(ih), 1, f);
        std::fwrite(px.data(), 4, px.size(), f);
        std::fclose(f);
    }
}

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

}  // namespace

int main(int argc, char** argv) {
    abyss::Game game;
    const char* model_path = argc > 1 ? argv[1] : std::getenv("AETHER_ABYSS_MODEL");
    std::vector<DrawTri> mesh;
    if (model_path && model_path[0]) {
        abyss::ModelInfo info;
        GltfScene scene;
        std::string error;
        if (LoadGltf(model_path, scene, &error)) {
            mesh = BuildMesh(scene);
            game.LoadModel(model_path, info);
            AETHER_LOG_INFO("Abyss", "Drew %d triangles from \"%s\" (%zu source vertices).",
                            static_cast<int>(mesh.size()), model_path, info.vertex_count);
        } else {
            AETHER_LOG_ERROR("Abyss", "glTF load failed: %s", error.c_str());
        }
    }

    WindowDesc desc;
    desc.title = "Abyss Protocol";
    desc.width = kW;
    desc.height = kH;
    Window window(desc);
    HWND hwnd = static_cast<HWND>(window.NativeHandle());

    i64 max_frames = -1;
    if (const char* env = std::getenv("AETHER_ABYSS_MAX_FRAMES")) max_frames = std::atoll(env);

    Renderer renderer;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kW;
    bmi.bmiHeader.biHeight = -kH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    f32 accumulator = 0, anim = 0, title_timer = 0;
    f32 yaw = 0.6f;
    f32 pitch = 0.35f;
    bool mouse_ready = false;
    bool cursor_hidden = false;
    bool prev_ping = false;
    bool prev_lights = false;
    bool prev_restart = false;
    bool lights = true;
    i64 frame = 0;

    while (window.PumpMessages()) {
        if (window.IsMinimized()) {
            Sleep(16);
            continue;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        f32 dt = std::min(0.1f, static_cast<f32>(now.QuadPart - last.QuadPart) / static_cast<f32>(freq.QuadPart));
        last = now;
        accumulator += dt;
        anim += dt;

        if (GetForegroundWindow() == hwnd) {
            if (Down(VK_ESCAPE)) break;
            if (Down(VK_LEFT)) yaw += 1.6f * dt;
            if (Down(VK_RIGHT)) yaw -= 1.6f * dt;
            if (Down(VK_UP)) pitch -= 1.0f * dt;
            if (Down(VK_DOWN)) pitch += 1.0f * dt;

            RECT rc;
            GetClientRect(hwnd, &rc);
            POINT centre{rc.right / 2, rc.bottom / 2};
            ClientToScreen(hwnd, &centre);
            POINT cur;
            GetCursorPos(&cur);
            if (mouse_ready) {
                yaw -= (cur.x - centre.x) * 0.003f;
                pitch += (cur.y - centre.y) * 0.003f;
            }
            SetCursorPos(centre.x, centre.y);
            mouse_ready = true;
            if (!cursor_hidden) {
                ShowCursor(FALSE);
                cursor_hidden = true;
            }
            pitch = std::clamp(pitch, -0.4f, 1.2f);

            bool ping = Down('F');
            if (ping && !prev_ping) game.Ping();
            prev_ping = ping;
            bool light_key = Down('L');
            if (light_key && !prev_lights) lights = !lights;
            prev_lights = light_key;
            bool restart = Down('R');
            if (restart && !prev_restart) {
                game.Restart();
                lights = true;
            }
            prev_restart = restart;
        } else {
            mouse_ready = false;
            if (cursor_hidden) {
                ShowCursor(TRUE);
                cursor_hidden = false;
            }
        }

        while (accumulator >= 1.0f / 60.0f) {
            f32 fwd = 0, side = 0, dive = 0;
            if (GetForegroundWindow() == hwnd) {
                fwd = (Down('W') ? 1.0f : 0.0f) - (Down('S') ? 1.0f : 0.0f);
                side = (Down('D') ? 1.0f : 0.0f) - (Down('A') ? 1.0f : 0.0f);
                dive = (Down('E') ? 1.0f : 0.0f) - (Down('Q') ? 1.0f : 0.0f);
            }
            f32 quiet = Down(VK_SHIFT) ? 0.28f : 1.0f;
            f32 fx = std::sin(yaw), fz = std::cos(yaw);
            game.SetLights(lights);
            game.ApplyThrust({(fx * fwd + fz * side) * 0.35f * quiet, dive * 0.22f * quiet,
                              (fz * fwd - fx * side) * 0.35f * quiet});
            game.Step(1.0f / 60.0f);
            accumulator -= 1.0f / 60.0f;
        }

        DrawWorld(game, mesh, renderer, yaw, pitch, anim);
        std::vector<u32> frame_px = renderer.Pixels();
        DrawHud(game, frame_px, !mesh.empty());

        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC dc = GetDC(hwnd);
        StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, kW, kH, frame_px.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd, dc);

        title_timer -= dt;
        if (title_timer <= 0) {
            title_timer = 0.25f;
            std::string status = game.StatusText();
            if (!mesh.empty()) status += "  [mesh]";
            SetWindowTextA(hwnd, status.c_str());
        }

        if (max_frames >= 0 && ++frame >= max_frames) {
            if (const char* shot = std::getenv("AETHER_ABYSS_SCREENSHOT")) SaveBmp(shot, frame_px);
            break;
        }
        Sleep(1);
    }
    return 0;
}
