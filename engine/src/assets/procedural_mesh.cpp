#include "aether/assets/procedural_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

namespace aether::assets::procedural {

namespace {

constexpr f32 kPi = 3.14159265358979f;

// --- Random numbers and noise: all integer hashing, so the same everywhere. ---

u32 Hash32(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
u32 Mix(u32 a, u32 b) { return Hash32(a * 0x9E3779B1u ^ Hash32(b + 0x7F4A7C15u)); }

class Rng {
public:
    explicit Rng(u32 seed) : state_(Hash32(seed) | 1u) {}
    u32 Next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }
    f32 Unit() { return static_cast<f32>(Next() >> 8) / 16777216.0f; }          // [0, 1)
    f32 Range(f32 lo, f32 hi) { return lo + (hi - lo) * Unit(); }
    i32 Int(i32 lo, i32 hi) { return lo + static_cast<i32>(Next() % static_cast<u32>(hi - lo + 1)); } // inclusive
    bool Chance(f32 p) { return Unit() < p; }

private:
    u32 state_;
};

f32 Lattice3(i32 x, i32 y, i32 z, u32 seed) {
    const u32 h = Hash32(static_cast<u32>(x) * 0x8DA6B343u ^ static_cast<u32>(y) * 0xD8163841u ^ static_cast<u32>(z) * 0xCB1AB31Fu ^ seed);
    return static_cast<f32>(h >> 8) / 8388608.0f - 1.0f; // -1..1
}
f32 Noise3(const Vec3& p, u32 seed) {
    const f32 fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const i32 ix = static_cast<i32>(fx), iy = static_cast<i32>(fy), iz = static_cast<i32>(fz);
    auto smooth = [](f32 t) { return t * t * (3.0f - 2.0f * t); };
    const f32 tx = smooth(p.x - fx), ty = smooth(p.y - fy), tz = smooth(p.z - fz);
    auto lerp = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
    f32 c[2][2][2];
    for (i32 dz = 0; dz < 2; ++dz)
        for (i32 dy = 0; dy < 2; ++dy)
            for (i32 dx = 0; dx < 2; ++dx) c[dz][dy][dx] = Lattice3(ix + dx, iy + dy, iz + dz, seed);
    f32 x0[2][2];
    for (i32 dz = 0; dz < 2; ++dz)
        for (i32 dy = 0; dy < 2; ++dy) x0[dz][dy] = lerp(c[dz][dy][0], c[dz][dy][1], tx);
    return lerp(lerp(x0[0][0], x0[0][1], ty), lerp(x0[1][0], x0[1][1], ty), tz);
}
f32 Fbm3(const Vec3& p, u32 octaves, u32 seed) {
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f;
    Vec3 q = p;
    for (u32 o = 0; o < octaves; ++o) {
        sum += Noise3(q, seed + o * 0x9E3779B9u) * amp;
        norm += amp;
        amp *= 0.5f;
        q = q * 2.0f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

f32 Smoothstep(f32 a, f32 b, f32 x) {
    const f32 t = std::clamp((x - a) / std::max(b - a, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

Vec3 Neg(const Vec3& v) { return Vec3(-v.x, -v.y, -v.z); }
Vec3 Lerp(const Vec3& a, const Vec3& b, f32 t) { return a + (b - a) * t; }

Vec3 Rotate(const Vec3& v, const Vec3& axis, f32 angle) {
    // Rodrigues.
    const f32 c = std::cos(angle), s = std::sin(angle);
    return v * c + axis.Cross(v) * s + axis * (axis.Dot(v) * (1.0f - c));
}

// Any unit vector perpendicular to `d` (a unit vector).
Vec3 Perpendicular(const Vec3& d) {
    const Vec3 helper = std::fabs(d.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    return d.Cross(helper).Normalized();
}

Vec3 Tint(const Vec3& base, Rng& rng, f32 amount) {
    const f32 k = 1.0f + (rng.Unit() * 2.0f - 1.0f) * amount;
    return Vec3(std::clamp(base.x * k, 0.0f, 1.0f), std::clamp(base.y * k, 0.0f, 1.0f), std::clamp(base.z * k, 0.0f, 1.0f));
}

MaterialData Material(const Vec3& color, f32 roughness, f32 metallic = 0.0f) {
    MaterialData m;
    m.base_color = Vec4(color.x, color.y, color.z, 1.0f);
    m.roughness = roughness;
    m.metallic = metallic;
    return m;
}

// The material with its wind sway set (metres at the top; 0 for none).
MaterialData Swaying(MaterialData material, f32 sway) {
    material.wind = std::clamp(sway, 0.0f, 0.3f);
    return material;
}

// Appends a primitive for `builder` (skipped when it has no triangles) and returns whether it did.
void AddPrimitive(ProceduralMesh& out, const MeshBuilder& builder, Shading shading, UvMode uv, const MaterialData& material) {
    if (builder.Empty()) return;
    out.materials.push_back(material);
    out.mesh.primitives.push_back(builder.Finish(shading, uv, static_cast<i32>(out.materials.size() - 1)));
}

} // namespace

u32 ProceduralMesh::TriangleCount() const {
    u32 n = 0;
    for (const MeshPrimitiveData& p : mesh.primitives) n += static_cast<u32>(p.indices.size() / 3);
    return n;
}
u32 ProceduralMesh::VertexCount() const {
    u32 n = 0;
    for (const MeshPrimitiveData& p : mesh.primitives) n += static_cast<u32>(p.vertices.size());
    return n;
}

// ======================================================================================================================================
// MeshBuilder

u32 MeshBuilder::Vertex(const Vec3& position, f32 u, f32 v) {
    positions_.push_back(position);
    uvs_.push_back(u);
    uvs_.push_back(v);
    return static_cast<u32>(positions_.size() - 1);
}

void MeshBuilder::Triangle(u32 a, u32 b, u32 c) {
    indices_.push_back(a);
    indices_.push_back(b);
    indices_.push_back(c);
}

void MeshBuilder::Quad(u32 a, u32 b, u32 c, u32 d) {
    Triangle(a, b, c);
    Triangle(a, c, d);
}

void MeshBuilder::Box(const Vec3& lo, const Vec3& hi) {
    const f32 sx = hi.x - lo.x, sy = hi.y - lo.y, sz = hi.z - lo.z;
    // Each face: four corners counter-clockwise seen from outside, with its own UV in metres.
    auto face = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, f32 w, f32 h) {
        Quad(Vertex(a, 0, 0), Vertex(b, w, 0), Vertex(c, w, h), Vertex(d, 0, h));
    };
    face(Vec3(lo.x, lo.y, hi.z), Vec3(hi.x, lo.y, hi.z), Vec3(hi.x, hi.y, hi.z), Vec3(lo.x, hi.y, hi.z), sx, sy); // +z
    face(Vec3(hi.x, lo.y, lo.z), Vec3(lo.x, lo.y, lo.z), Vec3(lo.x, hi.y, lo.z), Vec3(hi.x, hi.y, lo.z), sx, sy); // -z
    face(Vec3(hi.x, lo.y, hi.z), Vec3(hi.x, lo.y, lo.z), Vec3(hi.x, hi.y, lo.z), Vec3(hi.x, hi.y, hi.z), sz, sy); // +x
    face(Vec3(lo.x, lo.y, lo.z), Vec3(lo.x, lo.y, hi.z), Vec3(lo.x, hi.y, hi.z), Vec3(lo.x, hi.y, lo.z), sz, sy); // -x
    face(Vec3(lo.x, hi.y, hi.z), Vec3(hi.x, hi.y, hi.z), Vec3(hi.x, hi.y, lo.z), Vec3(lo.x, hi.y, lo.z), sx, sz); // +y
    face(Vec3(lo.x, lo.y, lo.z), Vec3(hi.x, lo.y, lo.z), Vec3(hi.x, lo.y, hi.z), Vec3(lo.x, lo.y, hi.z), sx, sz); // -y
}

void MeshBuilder::Tube(const std::vector<Vec3>& points, const std::vector<f32>& radii, u32 sides, bool cap_start, bool cap_end) {
    if (points.size() < 2 || radii.size() != points.size()) return;
    sides = std::max(sides, 3u);
    // Parallel transport: the ring's frame turns with the path, so the tube does not twist.
    Vec3 tangent = (points[1] - points[0]).Normalized();
    Vec3 side = Perpendicular(tangent);
    std::vector<std::vector<u32>> rings;
    f32 along = 0.0f;
    for (usize i = 0; i < points.size(); ++i) {
        if (i > 0) {
            along += (points[i] - points[i - 1]).Length();
            const Vec3 next = i + 1 < points.size() ? (points[i + 1] - points[i - 1]).Normalized() : (points[i] - points[i - 1]).Normalized();
            const Vec3 axis = tangent.Cross(next);
            const f32 s = axis.Length();
            if (s > 1e-5f) side = Rotate(side, axis * (1.0f / s), std::atan2(s, tangent.Dot(next)));
            tangent = next;
        }
        const Vec3 up = tangent.Cross(side).Normalized();
        std::vector<u32> ring;
        for (u32 k = 0; k <= sides; ++k) { // the extra vertex repeats the first with u = 1, so the UV seam is clean
            const f32 a = static_cast<f32>(k % sides) / static_cast<f32>(sides) * 2.0f * kPi;
            const Vec3 offset = side * std::cos(a) + up * std::sin(a);
            ring.push_back(Vertex(points[i] + offset * std::max(radii[i], 0.0f), static_cast<f32>(k) / static_cast<f32>(sides), along));
        }
        rings.push_back(ring);
    }
    for (usize i = 0; i + 1 < rings.size(); ++i) {
        for (u32 k = 0; k < sides; ++k) {
            // Seen from outside, going along the path with the side vector to the right: counter-clockwise a(k), a(k+1) upward.
            Quad(rings[i][k], rings[i][k + 1], rings[i + 1][k + 1], rings[i + 1][k]);
        }
    }
    auto cap = [&](const std::vector<u32>& ring, const Vec3& centre, bool start) {
        const u32 c = Vertex(centre, 0.5f, 0.5f);
        for (u32 k = 0; k < sides; ++k) {
            if (start) Triangle(c, ring[k + 1], ring[k]);
            else Triangle(c, ring[k], ring[k + 1]);
        }
    };
    if (cap_start) cap(rings.front(), points.front(), true);
    if (cap_end) cap(rings.back(), points.back(), false);
}

u32 MeshBuilder::IcoSphere(u32 subdivisions) {
    const u32 first = VertexCount();
    const f32 t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    std::vector<Vec3> v = {Vec3(-1, t, 0), Vec3(1, t, 0), Vec3(-1, -t, 0), Vec3(1, -t, 0), Vec3(0, -1, t), Vec3(0, 1, t),
                           Vec3(0, -1, -t), Vec3(0, 1, -t), Vec3(t, 0, -1), Vec3(t, 0, 1), Vec3(-t, 0, -1), Vec3(-t, 0, 1)};
    for (Vec3& p : v) p = p.Normalized();
    std::vector<std::array<u32, 3>> faces = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                             {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (u32 s = 0; s < std::min(subdivisions, 6u); ++s) {
        std::map<std::pair<u32, u32>, u32> midpoints;
        auto middle = [&](u32 a, u32 b) {
            const auto key = std::make_pair(std::min(a, b), std::max(a, b));
            const auto it = midpoints.find(key);
            if (it != midpoints.end()) return it->second;
            v.push_back(((v[a] + v[b]) * 0.5f).Normalized());
            return midpoints[key] = static_cast<u32>(v.size() - 1);
        };
        std::vector<std::array<u32, 3>> next;
        for (const auto& f : faces) {
            const u32 ab = middle(f[0], f[1]), bc = middle(f[1], f[2]), ca = middle(f[2], f[0]);
            next.push_back({f[0], ab, ca});
            next.push_back({f[1], bc, ab});
            next.push_back({f[2], ca, bc});
            next.push_back({ab, bc, ca});
        }
        faces.swap(next);
    }
    for (const Vec3& p : v) Vertex(p, 0.0f, 0.0f);
    for (const auto& f : faces) Triangle(first + f[0], first + f[1], first + f[2]);
    return first;
}

void MeshBuilder::Append(const MeshBuilder& other, const Vec3& offset) {
    const u32 base = VertexCount();
    for (usize i = 0; i < other.positions_.size(); ++i) Vertex(other.positions_[i] + offset, other.uvs_[i * 2], other.uvs_[i * 2 + 1]);
    for (const u32 index : other.indices_) indices_.push_back(base + index);
}

MeshPrimitiveData MeshBuilder::Finish(Shading shading, UvMode uv_mode, i32 material) const {
    MeshPrimitiveData out;
    out.material = material;
    const usize triangles = indices_.size() / 3;
    // Smooth normals: summed per welded position (to a millimetre), weighted by triangle area (the cross product's length).
    auto key_of = [](const Vec3& p) {
        auto q = [](f32 x) { return static_cast<i64>(std::llround(x * 1000.0f)); };
        return std::array<i64, 3>{q(p.x), q(p.y), q(p.z)};
    };
    std::map<std::array<i64, 3>, Vec3> sums;
    if (shading == Shading::Smooth) {
        for (usize t = 0; t < triangles; ++t) {
            const Vec3 &a = positions_[indices_[t * 3]], &b = positions_[indices_[t * 3 + 1]], &c = positions_[indices_[t * 3 + 2]];
            const Vec3 n = (b - a).Cross(c - a);
            for (int k = 0; k < 3; ++k) {
                Vec3& s = sums[key_of(positions_[indices_[t * 3 + static_cast<usize>(k)]])];
                s = s + n;
            }
        }
    }
    out.vertices.reserve(triangles * 3);
    out.indices.reserve(triangles * 3);
    std::vector<Vec3> tangent_sum, bitangent_sum;
    for (usize t = 0; t < triangles; ++t) {
        Vec3 p[3];
        f32 uv[3][2];
        for (int k = 0; k < 3; ++k) {
            const u32 i = indices_[t * 3 + static_cast<usize>(k)];
            p[k] = positions_[i];
            uv[k][0] = uvs_[i * 2];
            uv[k][1] = uvs_[i * 2 + 1];
        }
        const Vec3 face = (p[1] - p[0]).Cross(p[2] - p[0]);
        const Vec3 face_n = face.Normalized();
        if (uv_mode == UvMode::BoxProjected) {
            // The face's dominant axis picks the plane the texture is laid on (one repeat per metre).
            const f32 ax = std::fabs(face_n.x), ay = std::fabs(face_n.y), az = std::fabs(face_n.z);
            for (int k = 0; k < 3; ++k) {
                if (ax >= ay && ax >= az) { uv[k][0] = p[k].z; uv[k][1] = p[k].y; }
                else if (ay >= az) { uv[k][0] = p[k].x; uv[k][1] = p[k].z; }
                else { uv[k][0] = p[k].x; uv[k][1] = p[k].y; }
            }
        }
        // Tangent from the UV gradient.
        const Vec3 e1 = p[1] - p[0], e2 = p[2] - p[0];
        const f32 du1 = uv[1][0] - uv[0][0], dv1 = uv[1][1] - uv[0][1], du2 = uv[2][0] - uv[0][0], dv2 = uv[2][1] - uv[0][1];
        const f32 det = du1 * dv2 - du2 * dv1;
        Vec3 tangent = std::fabs(det) > 1e-12f ? (e1 * dv2 - e2 * dv1) * (1.0f / det) : Perpendicular(face_n);
        Vec3 bitangent = std::fabs(det) > 1e-12f ? (e2 * du1 - e1 * du2) * (1.0f / det) : face_n.Cross(tangent);
        for (int k = 0; k < 3; ++k) {
            Vec3 n = face_n;
            if (shading == Shading::Smooth) {
                const Vec3 s = sums[key_of(p[k])];
                if (s.LengthSq() > 1e-18f) n = s.Normalized();
            }
            Vec3 tn = tangent - n * n.Dot(tangent);
            tn = tn.LengthSq() > 1e-12f ? tn.Normalized() : Perpendicular(n);
            const f32 handed = n.Cross(tn).Dot(bitangent) < 0.0f ? -1.0f : 1.0f;
            MeshVertex v{};
            v.position[0] = p[k].x; v.position[1] = p[k].y; v.position[2] = p[k].z;
            v.normal[0] = n.x; v.normal[1] = n.y; v.normal[2] = n.z;
            v.uv[0] = uv[k][0]; v.uv[1] = uv[k][1];
            v.tangent[0] = tn.x; v.tangent[1] = tn.y; v.tangent[2] = tn.z; v.tangent[3] = handed;
            out.indices.push_back(static_cast<u32>(out.vertices.size()));
            out.vertices.push_back(v);
        }
    }
    if (!out.vertices.empty()) {
        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (const MeshVertex& v : out.vertices) {
            lo = Vec3(std::min(lo.x, v.position[0]), std::min(lo.y, v.position[1]), std::min(lo.z, v.position[2]));
            hi = Vec3(std::max(hi.x, v.position[0]), std::max(hi.y, v.position[1]), std::max(hi.z, v.position[2]));
        }
        out.bounds_min = lo;
        out.bounds_max = hi;
    }
    return out;
}

// ======================================================================================================================================
// Rocks

ProceduralMesh GenerateRock(const RockParams& in, u32 seed) {
    Rng rng(Mix(seed, 0x524F434Bu)); // "ROCK"
    const u32 noise_seed = rng.Next();
    const f32 size = std::max(in.size, 0.01f);
    MeshBuilder b;
    b.IcoSphere(std::clamp(in.subdivisions, 1u, 5u));

    // The stone's proportions: a long axis chosen at random, the others a little squashed.
    const f32 stretch = 1.0f + std::clamp(in.elongation, 0.0f, 1.0f);
    const f32 axis_scale[3] = {1.0f, 1.0f, 1.0f};
    f32 scale[3] = {axis_scale[0], axis_scale[1], axis_scale[2]};
    scale[rng.Int(0, 2)] = stretch;
    scale[0] *= rng.Range(0.85f, 1.15f);
    scale[1] *= rng.Range(0.7f, 1.0f); // stones sit lower than they are wide
    scale[2] *= rng.Range(0.85f, 1.15f);
    const f32 frequency = rng.Range(1.1f, 1.8f);
    const Vec3 offset(rng.Range(-50.0f, 50.0f), rng.Range(-50.0f, 50.0f), rng.Range(-50.0f, 50.0f));

    // Cut planes: each clips the stone's outside to a flat face at a random angle.
    struct Plane { Vec3 normal; f32 distance; };
    std::vector<Plane> cuts;
    for (u32 i = 0; i < in.cuts; ++i) {
        Vec3 n(rng.Range(-1, 1), rng.Range(-1, 1), rng.Range(-1, 1));
        if (n.LengthSq() < 1e-4f) n = Vec3(0, 1, 0);
        cuts.push_back({n.Normalized(), rng.Range(0.62f, 0.9f)});
    }
    const f32 floor_y = -1.0f + (1.0f - std::clamp(in.flatten_bottom, 0.0f, 1.0f)) * 0.9f;
    for (u32 i = 0; i < b.VertexCount(); ++i) {
        const Vec3 d = b.Position(i);
        f32 radius = 1.0f + in.roughness * Fbm3(d * frequency + offset, 4, noise_seed);
        Vec3 p = d * radius;
        for (const Plane& c : cuts) {
            const f32 over = p.Dot(c.normal) - c.distance;
            if (over > 0.0f) p = p - c.normal * over;
        }
        if (in.flatten_bottom > 0.0f && p.y < floor_y) p.y = floor_y;
        b.Position(i) = Vec3(p.x * scale[0], p.y * scale[1], p.z * scale[2]);
    }
    // Scale so the widest extent is `size`, and sit the stone on y = 0 with a centred footprint.
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (u32 i = 0; i < b.VertexCount(); ++i) {
        const Vec3& p = b.Position(i);
        lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
    const f32 widest = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-4f});
    const f32 k = size / widest;
    const Vec3 centre((lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f);
    for (u32 i = 0; i < b.VertexCount(); ++i) b.Position(i) = (b.Position(i) - centre) * k;

    ProceduralMesh out;
    AddPrimitive(out, b, in.shading, UvMode::BoxProjected, Material(in.color, 0.92f));
    return out;
}

// ======================================================================================================================================
// Cliffs

ProceduralMesh GenerateCliff(const CliffParams& in, u32 seed) {
    Rng rng(Mix(seed, 0x434C4946u)); // "CLIF"
    const u32 noise_seed = rng.Next();
    const u32 cols = std::clamp(in.columns, 4u, 400u), rows = std::clamp(in.rows, 4u, 400u);
    const f32 width = std::max(in.width, 0.1f), height = std::max(in.height, 0.1f), depth = std::max(in.depth, 0.05f);
    const u32 strata = std::clamp(in.strata, 1u, 40u);
    std::vector<f32> ledge(strata);
    for (f32& l : ledge) l = rng.Range(-1.0f, 1.0f) * in.overhang;
    const Vec3 offset(rng.Range(-40, 40), rng.Range(-40, 40), rng.Range(-40, 40));

    // How far the face stands out from the back plane at a point.
    auto face_depth = [&](f32 x, f32 y) {
        const f32 band_f = y / height * static_cast<f32>(strata);
        const u32 band = std::min(static_cast<u32>(band_f), strata - 1);
        const f32 within = band_f - static_cast<f32>(band);
        // A ledge sticks out and then drops back near the top of its band: a step, not a ramp.
        const f32 step = ledge[band] * (1.0f - Smoothstep(0.82f, 1.0f, within));
        // Fractures run vertically (a stretched noise), crumbling adds fine detail.
        const f32 fracture = Fbm3(Vec3(x * 0.9f, y * 0.12f, 0.0f) + offset, 3, noise_seed);
        const f32 crumble = Fbm3(Vec3(x * 0.4f, y * 0.4f, 7.0f) + offset, 4, noise_seed ^ 0xABCDu);
        const f32 base = 0.55f + 0.25f * fracture + 0.2f * crumble * in.roughness * 2.0f;
        // The slab rolls back to the back plane at its edges so it can sit against a slope.
        const f32 edge = Smoothstep(0.0f, 0.06f, std::min({x / width + 0.5f, 0.5f - x / width + 0.5f, y / height + 0.02f, 1.0f}));
        return std::clamp((base + step) * edge, 0.0f, 1.2f) * depth;
    };
    MeshBuilder b;
    std::vector<u32> grid((cols + 1) * (rows + 1));
    for (u32 r = 0; r <= rows; ++r) {
        for (u32 c = 0; c <= cols; ++c) {
            const f32 u = static_cast<f32>(c) / static_cast<f32>(cols), v = static_cast<f32>(r) / static_cast<f32>(rows);
            const f32 x = (u - 0.5f) * width, y = v * height;
            grid[r * (cols + 1) + c] = b.Vertex(Vec3(x, y, face_depth(x, y)), x, y);
        }
    }
    auto at = [&](u32 c, u32 r) { return grid[r * (cols + 1) + c]; };
    for (u32 r = 0; r < rows; ++r)
        for (u32 c = 0; c < cols; ++c) b.Quad(at(c, r), at(c + 1, r), at(c + 1, r + 1), at(c, r + 1)); // faces +z
    // The back and the closed edge: every boundary vertex gets a twin on the back plane (z = 0) and a wall between them.
    auto back = [&](u32 index) { Vec3 p = b.Position(index); return b.Vertex(Vec3(p.x, p.y, 0.0f), p.x, p.y); };
    std::vector<u32> bottom_front, bottom_back, top_front, top_back, left_front, left_back, right_front, right_back;
    for (u32 c = 0; c <= cols; ++c) {
        bottom_front.push_back(at(c, 0)); bottom_back.push_back(back(at(c, 0)));
        top_front.push_back(at(c, rows)); top_back.push_back(back(at(c, rows)));
    }
    for (u32 r = 0; r <= rows; ++r) {
        left_front.push_back(at(0, r)); left_back.push_back(back(at(0, r)));
        right_front.push_back(at(cols, r)); right_back.push_back(back(at(cols, r)));
    }
    for (u32 c = 0; c < cols; ++c) {
        b.Quad(bottom_back[c], bottom_back[c + 1], bottom_front[c + 1], bottom_front[c]);         // underside, faces -y
        b.Quad(top_front[c], top_front[c + 1], top_back[c + 1], top_back[c]);                     // top, faces +y
    }
    for (u32 r = 0; r < rows; ++r) {
        b.Quad(left_front[r], left_front[r + 1], left_back[r + 1], left_back[r]);                 // left, faces -x
        b.Quad(right_back[r], right_back[r + 1], right_front[r + 1], right_front[r]);             // right, faces +x
    }
    // The back plane, faces -z: a grid of its own so its vertices line up with the walls.
    std::vector<u32> back_grid((cols + 1) * (rows + 1));
    for (u32 r = 0; r <= rows; ++r)
        for (u32 c = 0; c <= cols; ++c) back_grid[r * (cols + 1) + c] = back(at(c, r));
    for (u32 r = 0; r < rows; ++r)
        for (u32 c = 0; c < cols; ++c)
            b.Quad(back_grid[r * (cols + 1) + c + 1], back_grid[r * (cols + 1) + c], back_grid[(r + 1) * (cols + 1) + c], back_grid[(r + 1) * (cols + 1) + c + 1]);
    ProceduralMesh out;
    AddPrimitive(out, b, Shading::Smooth, UvMode::BoxProjected, Material(in.color, 0.95f));
    return out;
}

// ======================================================================================================================================
// Trees

namespace {

// A leaf mass: an icosphere pushed into a lumpy blob around `centre`.
void LeafBlob(MeshBuilder& b, const Vec3& centre, f32 radius, u32 detail, Rng& rng) {
    MeshBuilder blob;
    blob.IcoSphere(detail);
    const u32 seed = rng.Next();
    const Vec3 squash(rng.Range(0.9f, 1.2f), rng.Range(0.7f, 0.95f), rng.Range(0.9f, 1.2f));
    for (u32 i = 0; i < blob.VertexCount(); ++i) {
        const Vec3 d = blob.Position(i);
        const f32 r = radius * (1.0f + 0.28f * Noise3(d * 1.6f, seed));
        blob.Position(i) = Vec3(d.x * r * squash.x, d.y * r * squash.y, d.z * r * squash.z);
    }
    b.Append(blob, centre);
}

struct BranchContext {
    MeshBuilder* wood;
    MeshBuilder* leaves;
    Rng* rng;
    u32 sides;
    u32 detail;
    f32 foliage;
    f32 leaf_size;
    u32 max_levels;
    bool bare;
    f32 trunk_radius;
};

void GrowBranch(BranchContext& ctx, const Vec3& start, const Vec3& direction, f32 length, f32 radius, u32 level) {
    Rng& rng = *ctx.rng;
    const u32 segments = level == 0 ? 5u : 3u; // the thin twigs need fewer bends
    std::vector<Vec3> points;
    std::vector<f32> radii;
    Vec3 dir = direction.Normalized();
    Vec3 p = start;
    const Vec3 wobble_axis = Perpendicular(dir);
    for (u32 s = 0; s <= segments; ++s) {
        points.push_back(p);
        const f32 t = static_cast<f32>(s) / static_cast<f32>(segments);
        radii.push_back(radius * (1.0f - 0.7f * t));
        // The branch bends a little and reaches toward the light.
        dir = (dir + wobble_axis * rng.Range(-0.12f, 0.12f) + Vec3(0, 0.06f, 0)).Normalized();
        p = p + dir * (length / static_cast<f32>(segments));
    }
    // A last point at a hair's width closes the tip.
    ctx.wood->Tube(points, radii, std::max(ctx.sides > level * 2 ? ctx.sides - level * 2 : 0u, 4u), false, true);
    const Vec3 tip = points.back();
    if (level >= ctx.max_levels) {
        if (!ctx.bare && ctx.foliage > 0.0f) {
            const u32 masses = 1 + (rng.Chance(ctx.foliage * 0.6f) ? 1u : 0u);
            for (u32 m = 0; m < masses; ++m) {
                const Vec3 jitter(rng.Range(-0.3f, 0.3f) * ctx.leaf_size, rng.Range(0.0f, 0.4f) * ctx.leaf_size, rng.Range(-0.3f, 0.3f) * ctx.leaf_size);
                LeafBlob(*ctx.leaves, tip + jitter, ctx.leaf_size * rng.Range(0.8f, 1.2f), ctx.detail, rng);
            }
        }
        return;
    }
    const u32 children = static_cast<u32>(rng.Int(2, 3));
    for (u32 c = 0; c < children; ++c) {
        const f32 along = rng.Range(0.55f, 1.0f);
        const usize idx = std::min(static_cast<usize>(along * static_cast<f32>(segments)), static_cast<usize>(segments));
        const Vec3 origin = points[idx];
        // Fan the children around the parent, each tipped 25 to 55 degrees away from it.
        const Vec3 side = Rotate(Perpendicular(dir), dir, rng.Range(0.0f, 2.0f * kPi));
        const f32 tilt = rng.Range(0.45f, 0.95f);
        const Vec3 child = (dir * std::cos(tilt) + side * std::sin(tilt)).Normalized();
        GrowBranch(ctx, origin, child, length * rng.Range(0.55f, 0.75f), radii[idx] * 0.75f, level + 1);
    }
}

} // namespace

ProceduralMesh GenerateTree(const TreeParams& in, u32 seed) {
    Rng rng(Mix(seed, 0x54524545u)); // "TREE"
    const f32 height = std::max(in.height, 0.5f);
    const f32 trunk_radius = std::clamp(in.trunk_radius, 0.01f, height * 0.5f);
    const u32 sides = std::clamp(in.sides, 4u, 24u);
    MeshBuilder wood, leaves;

    // The trunk's centreline sways with the lean and a slow wobble.
    const f32 trunk_fraction = in.species == TreeSpecies::Pine ? 0.97f : (in.species == TreeSpecies::Dead ? 0.7f : 0.45f);
    const f32 trunk_height = height * trunk_fraction;
    const Vec3 sway_dir = Vec3(rng.Range(-1, 1), 0, rng.Range(-1, 1)).Normalized();
    const f32 lean = std::clamp(in.lean, 0.0f, 0.5f) * height;
    const f32 wobble_phase = rng.Range(0.0f, 6.28f), wobble = trunk_radius * 0.6f;
    std::vector<Vec3> trunk;
    std::vector<f32> radius;
    const u32 segments = 10;
    for (u32 i = 0; i <= segments; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(segments);
        const Vec3 swing = sway_dir * (lean * t * t) + Perpendicular(sway_dir) * (std::sin(t * 5.0f + wobble_phase) * wobble * t);
        trunk.push_back(Vec3(swing.x, t * trunk_height, swing.z));
        // A flared root, then a taper.
        const f32 flare = 1.0f + 0.9f * std::exp(-t * 18.0f);
        const f32 taper = in.species == TreeSpecies::Pine ? (1.0f - 0.92f * t) : (1.0f - 0.55f * t);
        radius.push_back(trunk_radius * flare * taper);
    }
    // Sink the base a little so the roots meet a slope without a gap.
    trunk.front().y -= 0.15f;
    wood.Tube(trunk, radius, sides, false, true);

    if (in.species == TreeSpecies::Pine) {
        // Tiers of drooping cones up the upper trunk.
        const u32 tiers = std::clamp(static_cast<u32>(std::lround(4.0 + height * 0.5 * std::max(in.foliage, 0.2f))), 3u, 14u);
        const f32 base_y = height * 0.18f;
        const f32 top_y = height;
        const f32 max_radius = height * 0.22f * std::max(in.foliage, 0.3f);
        const u32 rim = 12;
        for (u32 tier = 0; tier < tiers; ++tier) {
            const f32 t = static_cast<f32>(tier) / static_cast<f32>(tiers);
            const f32 y = base_y + (top_y - base_y) * t * 0.97f;
            const f32 tier_height = (top_y - base_y) / static_cast<f32>(tiers) * 2.6f;
            const f32 r = max_radius * (1.0f - t) * rng.Range(0.85f, 1.1f) + 0.05f;
            // Trunk centre at this height.
            const f32 tt = std::clamp(y / trunk_height, 0.0f, 1.0f);
            const Vec3 swing = sway_dir * (lean * tt * tt);
            const Vec3 centre(swing.x, y, swing.z);
            const u32 apex = leaves.Vertex(centre + Vec3(0, tier_height, 0), 0.5f, 1.0f);
            const u32 hub = leaves.Vertex(centre + Vec3(0, tier_height * 0.12f, 0), 0.5f, 0.0f);
            std::vector<u32> ring;
            const f32 phase = rng.Range(0.0f, 6.28f);
            for (u32 k = 0; k <= rim; ++k) {
                const f32 a = phase + static_cast<f32>(k % rim) / static_cast<f32>(rim) * 2.0f * kPi;
                const f32 jag = 1.0f + 0.22f * ((k % 2 == 0) ? 1.0f : -1.0f) * rng.Range(0.4f, 1.0f); // points and notches round the skirt
                ring.push_back(leaves.Vertex(centre + Vec3(std::cos(a) * r * jag, -tier_height * 0.1f * rng.Range(0.6f, 1.2f), std::sin(a) * r * jag),
                                             static_cast<f32>(k) / static_cast<f32>(rim), 0.4f));
            }
            for (u32 k = 0; k < rim; ++k) {
                // Counter-clockwise from outside: the angle grows toward -z in this basis, so the top triangle runs ring[k+1], ring[k].
                leaves.Triangle(apex, ring[k + 1], ring[k]);
                leaves.Triangle(hub, ring[k], ring[k + 1]);
            }
        }
    } else {
        BranchContext ctx{&wood, &leaves, &rng, sides, std::min(in.foliage_detail, 2u), in.foliage, 0.0f, std::clamp(in.branch_levels, 1u, 5u),
                          in.species == TreeSpecies::Dead, trunk_radius};
        ctx.leaf_size = height * 0.16f * std::clamp(in.foliage, 0.3f, 1.5f);
        const Vec3 top = trunk.back();
        const f32 crown = height - trunk_height;
        const u32 mains = static_cast<u32>(rng.Int(in.species == TreeSpecies::Dead ? 3 : 4, in.species == TreeSpecies::Dead ? 5 : 6));
        // A crown of many twigs takes plain leaf masses; a sparse one can afford rounder ones (about 60 masses is the budget for detail 1).
        const f32 masses = static_cast<f32>(mains) * std::pow(2.5f, static_cast<f32>(ctx.max_levels)) * 1.6f;
        if (masses > 60.0f) ctx.detail = 0;
        for (u32 m = 0; m < mains; ++m) {
            // Main limbs start up the upper trunk and fan outward and up.
            const f32 along = rng.Range(0.55f, 1.0f);
            const usize idx = std::min(static_cast<usize>(along * static_cast<f32>(segments)), static_cast<usize>(segments));
            const f32 a = (static_cast<f32>(m) + rng.Range(-0.3f, 0.3f)) / static_cast<f32>(mains) * 2.0f * kPi;
            const f32 up = rng.Range(0.5f, 1.1f);
            const Vec3 dir = Vec3(std::cos(a), up, std::sin(a)).Normalized();
            GrowBranch(ctx, trunk[idx], dir, crown * rng.Range(0.7f, 1.0f) + height * 0.12f, radius[idx] * 0.6f, 0);
        }
        // The leader: the trunk's own continuation, with a mass of leaves on top.
        if (!ctx.bare) LeafBlob(leaves, top + Vec3(0, ctx.leaf_size * 0.3f, 0), ctx.leaf_size * 1.3f, ctx.detail, rng);
    }

    // Branches and tiers grow where the dice put them; scale the whole tree so its tip is at the height that was asked for.
    f32 top = 1e-3f;
    for (u32 i = 0; i < wood.VertexCount(); ++i) top = std::max(top, wood.Position(i).y);
    for (u32 i = 0; i < leaves.VertexCount(); ++i) top = std::max(top, leaves.Position(i).y);
    const f32 fit = height / top;
    for (u32 i = 0; i < wood.VertexCount(); ++i) wood.Position(i) = wood.Position(i) * fit;
    for (u32 i = 0; i < leaves.VertexCount(); ++i) leaves.Position(i) = leaves.Position(i) * fit;

    ProceduralMesh out;
    AddPrimitive(out, wood, Shading::Smooth, UvMode::Given, Material(in.bark_color, 0.95f));
    AddPrimitive(out, leaves, Shading::Smooth, UvMode::BoxProjected, Swaying(Material(in.leaf_color, 0.8f), in.sway));
    return out;
}

// ======================================================================================================================================
// Plants

namespace {
// Scales every vertex of the builders about the origin so the highest point is at `height`.
void FitHeight(f32 height, std::initializer_list<MeshBuilder*> builders) {
    f32 top = 1e-3f;
    for (MeshBuilder* b : builders)
        for (u32 i = 0; i < b->VertexCount(); ++i) top = std::max(top, b->Position(i).y);
    const f32 k = height / top;
    for (MeshBuilder* b : builders)
        for (u32 i = 0; i < b->VertexCount(); ++i) b->Position(i) = b->Position(i) * k;
}
} // namespace

ProceduralMesh GenerateShrub(const ShrubParams& in, u32 seed) {
    Rng rng(Mix(seed, 0x53485242u)); // "SHRB"
    const f32 height = std::max(in.height, 0.1f);
    const u32 masses = std::clamp(in.masses, 3u, 14u);
    const f32 radius = height * std::max(in.spread, 0.3f) * 0.5f;
    MeshBuilder leaves, wood;
    std::vector<Vec3> centres;
    for (u32 i = 0; i < masses; ++i) {
        // Low and wide: masses ring the clump, a few rise at the middle.
        const f32 a = rng.Range(0.0f, 2.0f * kPi), r = std::sqrt(rng.Unit()) * radius;
        const f32 y = height * rng.Range(0.3f, 0.75f) * (1.0f - 0.5f * r / std::max(radius, 1e-3f) + 0.5f);
        centres.push_back(Vec3(std::cos(a) * r, y, std::sin(a) * r));
        LeafBlob(leaves, centres.back(), height * rng.Range(0.22f, 0.36f), std::min(in.detail, 1u), rng);
    }
    const u32 stems = static_cast<u32>(std::max(in.stems, 0.0f));
    for (u32 s = 0; s < stems; ++s) {
        const Vec3 target = centres[s % centres.size()];
        const Vec3 base(target.x * 0.3f, -0.03f, target.z * 0.3f);
        wood.Tube({base, Lerp(base, target, 0.5f) + Vec3(rng.Range(-0.04f, 0.04f), 0, rng.Range(-0.04f, 0.04f)), target}, {height * 0.025f, height * 0.018f, height * 0.01f}, 4, false, true);
    }
    FitHeight(height, {&leaves, &wood});
    ProceduralMesh out;
    AddPrimitive(out, wood, Shading::Smooth, UvMode::Given, Material(in.stem_color, 0.95f));
    AddPrimitive(out, leaves, Shading::Smooth, UvMode::BoxProjected, Swaying(Material(in.leaf_color, 0.8f), in.sway));
    return out;
}

ProceduralMesh GenerateGrass(const GrassParams& in, u32 seed) {
    Rng rng(Mix(seed, 0x47524153u)); // "GRAS"
    const f32 height = std::max(in.height, 0.05f);
    const u32 blades = std::clamp(in.blades, 3u, 40u);
    MeshBuilder b;
    constexpr u32 rows = 3; // segments up each blade
    for (u32 k = 0; k < blades; ++k) {
        const f32 a = rng.Range(0.0f, 2.0f * kPi), r = std::sqrt(rng.Unit()) * in.spread;
        const Vec3 root(std::cos(a) * r, 0.0f, std::sin(a) * r);
        const f32 h = height * rng.Range(0.55f, 1.0f);
        const f32 facing = rng.Range(0.0f, 2.0f * kPi);
        const Vec3 lean_dir(std::cos(facing), 0.0f, std::sin(facing));
        const Vec3 side(-lean_dir.z, 0.0f, lean_dir.x);
        const f32 lean = h * in.curl * rng.Range(0.3f, 1.0f), w = in.width * rng.Range(0.7f, 1.2f);
        // A ribbon whose centre line bends over: up first, then more and more along its lean.
        std::vector<u32> left, right;
        for (u32 s = 0; s <= rows; ++s) {
            const f32 t = static_cast<f32>(s) / static_cast<f32>(rows);
            const Vec3 centre = root + lean_dir * (lean * t * t) + Vec3(0, h * (1.0f - 0.25f * t * t) * t, 0);
            const f32 half = 0.5f * w * (1.0f - 0.92f * t); // tapering to a point
            left.push_back(b.Vertex(centre - side * half, 0.0f, t));
            right.push_back(b.Vertex(centre + side * half, 1.0f, t));
        }
        for (u32 s = 0; s < rows; ++s) {
            // Both faces, so the blade shows from either side whatever the culling.
            b.Quad(left[s], right[s], right[s + 1], left[s + 1]);
            b.Quad(left[s], left[s + 1], right[s + 1], right[s]);
        }
    }
    ProceduralMesh out;
    AddPrimitive(out, b, Shading::Flat, UvMode::Given, Swaying(Material(in.color, 0.85f), in.sway));
    return out;
}

ProceduralMesh GenerateReeds(const ReedParams& in, u32 seed) {
    Rng rng(Mix(seed, 0x52454544u)); // "REED"
    const f32 height = std::max(in.height, 0.2f);
    const u32 stems = std::clamp(in.stems, 2u, 20u);
    MeshBuilder stalks, heads;
    for (u32 k = 0; k < stems; ++k) {
        const f32 a = rng.Range(0.0f, 2.0f * kPi), r = std::sqrt(rng.Unit()) * in.spread;
        const Vec3 root(std::cos(a) * r, -0.02f, std::sin(a) * r);
        const f32 h = height * rng.Range(0.6f, 1.0f);
        const f32 lean_a = rng.Range(0.0f, 2.0f * kPi), lean = h * rng.Range(0.02f, 0.14f);
        const Vec3 lean_dir(std::cos(lean_a), 0, std::sin(lean_a));
        std::vector<Vec3> path;
        std::vector<f32> radii;
        for (u32 s = 0; s <= 4; ++s) {
            const f32 t = static_cast<f32>(s) / 4.0f;
            path.push_back(root + lean_dir * (lean * t * t) + Vec3(0, h * t, 0));
            radii.push_back(0.012f * (1.0f - 0.55f * t));
        }
        stalks.Tube(path, radii, 4, false, true);
        if (in.heads && rng.Chance(0.55f)) {
            // A cattail: a short fat cylinder just under the tip.
            const Vec3 top = path.back(), below = path[3] + (path[4] - path[3]) * 0.35f;
            heads.Tube({below, top}, {0.028f, 0.02f}, 6, true, true);
        }
    }
    ProceduralMesh out;
    AddPrimitive(out, stalks, Shading::Smooth, UvMode::Given, Swaying(Material(in.stem_color, 0.8f), in.sway));
    AddPrimitive(out, heads, Shading::Smooth, UvMode::Given, Swaying(Material(in.head_color, 0.9f), in.sway));
    return out;
}

ProceduralMesh GenerateCactus(const CactusParams& in, u32 seed) {
    Rng rng(Mix(seed, 0x43414354u)); // "CACT"
    const f32 height = std::max(in.height, 0.3f), radius = std::clamp(in.radius, 0.02f, height * 0.4f);
    const u32 sides = std::clamp(in.sides, 5u, 16u);
    MeshBuilder b;
    // The column: a rounded top (the radius closes over the last rings).
    std::vector<Vec3> path;
    std::vector<f32> radii;
    const u32 rings = 9;
    for (u32 i = 0; i <= rings; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(rings);
        path.push_back(Vec3(0, t * height, 0));
        const f32 cap = t > 0.85f ? std::sqrt(std::max(1.0f - (t - 0.85f) / 0.15f * (t - 0.85f) / 0.15f * 0.97f, 0.0f)) : 1.0f;
        radii.push_back(radius * cap * (1.0f + 0.12f * std::exp(-t * 14.0f)));
    }
    b.Tube(path, radii, sides, false, true);
    const u32 arms = std::min(in.arms, 4u);
    for (u32 k = 0; k < arms; ++k) {
        // An arm leaves the column sideways and turns upward: out, a bend, then up.
        const f32 a = (static_cast<f32>(k) + rng.Range(-0.25f, 0.25f)) / static_cast<f32>(std::max(arms, 1u)) * 2.0f * kPi;
        const Vec3 out_dir(std::cos(a), 0, std::sin(a));
        const f32 y0 = height * rng.Range(0.3f, 0.6f), reach = height * rng.Range(0.18f, 0.3f), rise = height * rng.Range(0.2f, 0.34f), ar = radius * rng.Range(0.55f, 0.7f);
        const Vec3 start = out_dir * (radius * 0.4f) + Vec3(0, y0, 0);
        const std::vector<Vec3> arm = {start, start + out_dir * (reach * 0.6f), start + out_dir * reach + Vec3(0, rise * 0.1f, 0), start + out_dir * (reach + ar * 0.5f) + Vec3(0, rise * 0.55f, 0),
                                       start + out_dir * (reach + ar * 0.5f) + Vec3(0, rise, 0)};
        b.Tube(arm, {ar, ar, ar, ar * 0.95f, ar * 0.6f}, std::max(sides - 2, 5u), false, true);
    }
    ProceduralMesh out;
    AddPrimitive(out, b, Shading::Smooth, UvMode::Given, Material(in.color, 0.7f));
    return out;
}

// ======================================================================================================================================
// Buildings and modular pieces

namespace {

// An opening cut through a wall: a range along the wall and a range in height. A glass opening gets a pane and a frame; the door does not.
struct Opening { f32 a0, a1, y0, y1; bool glass; };

// Boxes in a wall's own axes: `a` along the wall, `y` up, `n` through it. With along_z the wall runs along z and n is x.
void WallBox(MeshBuilder& b, bool along_z, f32 a0, f32 a1, f32 y0, f32 y1, f32 n0, f32 n1) {
    if (along_z) b.Box(Vec3(n0, y0, a0), Vec3(n1, y1, a1));
    else b.Box(Vec3(a0, y0, n0), Vec3(a1, y1, n1));
}

// A wall from a0 to a1 and y0 to y1, between n_in and n_out through its thickness. It is split into columns at the openings' edges; a
// column between openings is a full block, one at an opening is a sill below and a lintel above. Panes sit mid-wall; trim frames them on the
// outer face (the larger |n|).
void WallWithOpenings(MeshBuilder& wall, MeshBuilder& glass, MeshBuilder& trim, bool along_z, f32 a0, f32 a1, f32 y0, f32 y1, f32 n_in, f32 n_out,
                      const std::vector<Opening>& openings) {
    std::vector<f32> cuts = {a0, a1};
    for (const Opening& o : openings) { cuts.push_back(std::clamp(o.a0, a0, a1)); cuts.push_back(std::clamp(o.a1, a0, a1)); }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(), [](f32 x, f32 y) { return std::fabs(x - y) < 1e-4f; }), cuts.end());
    const f32 outer = std::fabs(n_out) > std::fabs(n_in) ? n_out : n_in;
    const f32 out_sign = outer >= 0.0f ? 1.0f : -1.0f;
    for (usize i = 0; i + 1 < cuts.size(); ++i) {
        const f32 a = cuts[i], c = cuts[i + 1], mid = (a + c) * 0.5f;
        const Opening* hole = nullptr;
        for (const Opening& o : openings) if (mid > o.a0 && mid < o.a1) hole = &o;
        if (!hole) { WallBox(wall, along_z, a, c, y0, y1, n_in, n_out); continue; }
        if (hole->y0 > y0 + 1e-4f) WallBox(wall, along_z, a, c, y0, hole->y0, n_in, n_out);
        if (hole->y1 < y1 - 1e-4f) WallBox(wall, along_z, a, c, hole->y1, y1, n_in, n_out);
        if (!hole->glass) continue;
        const f32 nm = (n_in + n_out) * 0.5f;
        WallBox(glass, along_z, a, c, hole->y0, hole->y1, nm - 0.01f, nm + 0.01f);
        // Sill and header trim on the outside face; the side posts of the opening are the wall's own columns.
        const f32 f = 0.07f, lo = outer - out_sign * 0.02f, hi = outer + out_sign * 0.05f;
        WallBox(trim, along_z, a - f, c + f, hole->y0 - f, hole->y0, std::min(lo, hi), std::max(lo, hi));
        WallBox(trim, along_z, a - f, c + f, hole->y1, hole->y1 + f, std::min(lo, hi), std::max(lo, hi));
    }
}

} // namespace

ProceduralMesh GenerateBuilding(const BuildingParams& in, u32 seed) {
    Rng rng(Mix(seed, 0x424C4447u)); // "BLDG"
    const u32 floors = std::clamp(in.floors, 1u, 12u);
    const u32 bx = std::clamp(in.bays_x, 1u, 16u), bz = std::clamp(in.bays_z, 1u, 16u);
    const f32 bay = std::max(in.bay_size, 1.0f), fh = std::max(in.floor_height, 2.0f);
    const f32 width = bay * static_cast<f32>(bx), depth = bay * static_cast<f32>(bz), height = fh * static_cast<f32>(floors);
    const f32 t = 0.25f; // wall thickness
    const f32 hx = width * 0.5f, hz = depth * 0.5f;
    MeshBuilder walls, roof, glass, trim;

    for (u32 floor = 0; floor < floors; ++floor) {
        const f32 y0 = static_cast<f32>(floor) * fh, y1 = y0 + fh;
        // Openings for a wall of `bays` bays starting at `start` along its axis; the front's middle bay is the door on the ground floor.
        auto openings = [&](u32 bays, f32 start, bool door_here) {
            std::vector<Opening> o;
            for (u32 k = 0; k < bays; ++k) {
                const f32 a = start + static_cast<f32>(k) * bay, mid = a + bay * 0.5f;
                if (door_here && floor == 0 && k == bays / 2) o.push_back({mid - 0.55f, mid + 0.55f, y0, y0 + 2.2f, false});
                else o.push_back({mid - bay * 0.22f, mid + bay * 0.22f, y0 + fh * 0.35f, y0 + fh * 0.8f, true});
            }
            return o;
        };
        WallWithOpenings(walls, glass, trim, false, -hx, hx, y0, y1, hz - t, hz, openings(bx, -hx, in.door));      // front, +z
        WallWithOpenings(walls, glass, trim, false, -hx, hx, y0, y1, -hz, -hz + t, openings(bx, -hx, false));     // back, -z
        // The sides fit between the front and back walls, so their bays are measured from the inside corners.
        const f32 side_bay = (depth - 2.0f * t) / static_cast<f32>(bz);
        auto side_openings = [&]() {
            std::vector<Opening> o;
            for (u32 k = 0; k < bz; ++k) {
                const f32 mid = -hz + t + (static_cast<f32>(k) + 0.5f) * side_bay;
                o.push_back({mid - bay * 0.22f, mid + bay * 0.22f, y0 + fh * 0.35f, y0 + fh * 0.8f, true});
            }
            return o;
        };
        WallWithOpenings(walls, glass, trim, true, -hz + t, hz - t, y0, y1, hx - t, hx, side_openings());          // right, +x
        WallWithOpenings(walls, glass, trim, true, -hz + t, hz - t, y0, y1, -hx, -hx + t, side_openings());        // left, -x
        // The floor slab (the ground floor's is the foundation).
        walls.Box(Vec3(-hx + t, y0, -hz + t), Vec3(hx - t, y0 + 0.15f, hz - t));
    }
    // A door slab and a step in front of it.
    if (in.door) {
        trim.Box(Vec3(-0.5f, 0.0f, hz - t * 0.6f), Vec3(0.5f, 2.15f, hz - t * 0.6f + 0.06f));
        walls.Box(Vec3(-1.0f, 0.0f, hz), Vec3(1.0f, 0.12f, hz + 0.7f));
    }

    // The roof.
    const f32 overhang = 0.45f;
    if (in.roof == RoofType::Flat) {
        roof.Box(Vec3(-hx - 0.1f, height, -hz - 0.1f), Vec3(hx + 0.1f, height + 0.2f, hz + 0.1f));
        const f32 p = 0.15f, ph = 0.6f; // a parapet round the edge
        roof.Box(Vec3(-hx - 0.1f, height + 0.2f, hz + 0.1f - p), Vec3(hx + 0.1f, height + 0.2f + ph, hz + 0.1f));
        roof.Box(Vec3(-hx - 0.1f, height + 0.2f, -hz - 0.1f), Vec3(hx + 0.1f, height + 0.2f + ph, -hz - 0.1f + p));
        roof.Box(Vec3(hx + 0.1f - p, height + 0.2f, -hz - 0.1f + p), Vec3(hx + 0.1f, height + 0.2f + ph, hz + 0.1f - p));
        roof.Box(Vec3(-hx - 0.1f, height + 0.2f, -hz - 0.1f + p), Vec3(-hx - 0.1f + p, height + 0.2f + ph, hz + 0.1f - p));
    } else {
        // A gable along x: a ridge over the middle of the depth. A closed prism with the eaves' overhang.
        const f32 rise = std::max(depth * 0.32f, 1.0f), x_out = hx + overhang, z_out = hz + overhang, th = 0.18f;
        const f32 y_eave = height, y_ridge = height + rise * (z_out / hz);
        const Vec3 a(-x_out, y_eave, z_out), b(x_out, y_eave, z_out), c(x_out, y_ridge, 0), d(-x_out, y_ridge, 0);   // front slope
        const Vec3 e(-x_out, y_eave, -z_out), f(x_out, y_eave, -z_out);                                              // back eave
        // Slope surfaces face up and out; undersides are the same quads one thickness lower, facing down.
        auto slab = [&](const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, const Vec3& drop) {
            const Vec3 q0 = p0 - drop, q1 = p1 - drop, q2 = p2 - drop, q3 = p3 - drop;
            roof.Quad(roof.Vertex(p0, 0, 0), roof.Vertex(p1, 2.0f * x_out, 0), roof.Vertex(p2, 2.0f * x_out, 3), roof.Vertex(p3, 0, 3));         // top
            roof.Quad(roof.Vertex(q3, 0, 3), roof.Vertex(q2, 2.0f * x_out, 3), roof.Vertex(q1, 2.0f * x_out, 0), roof.Vertex(q0, 0, 0));         // underside
            roof.Quad(roof.Vertex(q0, 0, 0), roof.Vertex(q1, 1, 0), roof.Vertex(p1, 1, th), roof.Vertex(p0, 0, th));                              // eave edge
            roof.Quad(roof.Vertex(p3, 0, 0), roof.Vertex(p2, 1, 0), roof.Vertex(q2, 1, th), roof.Vertex(q3, 0, th));                              // ridge edge
            roof.Quad(roof.Vertex(q0, 0, 0), roof.Vertex(p0, 1, 0), roof.Vertex(p3, 1, 3), roof.Vertex(q3, 0, 3));                                // left end
            roof.Quad(roof.Vertex(p1, 0, 0), roof.Vertex(q1, 1, 0), roof.Vertex(q2, 1, 3), roof.Vertex(p2, 0, 3));                                // right end
        };
        // Front slope: ordering (a, b, c, d) is counter-clockwise from above/outside (a to b along +x, then up the slope to the ridge).
        slab(a, b, c, d, Vec3(0, th, 0));
        // Back slope: mirrored in z, ordering reversed to face the other way.
        const Vec3 c2(x_out, y_ridge, 0), d2(-x_out, y_ridge, 0);
        slab(f, e, d2, c2, Vec3(0, th, 0));
        // The gable ends: triangles filling the wall up to the roof, one at each end, set just inside the roof.
        for (const f32 sign : {1.0f, -1.0f}) {
            const f32 x = sign * (hx - 0.02f);
            const f32 inset = hz - t;
            const f32 ridge_y = height + rise * (inset / hz) * 0.0f + (y_ridge - height) * 0.0f + rise; // the wall's own ridge height
            const u32 v0 = walls.Vertex(Vec3(x, height, inset), 0, 0), v1 = walls.Vertex(Vec3(x, height, -inset), 2 * inset, 0), v2 = walls.Vertex(Vec3(x, ridge_y, 0), inset, rise);
            if (sign > 0) walls.Triangle(v0, v1, v2); else walls.Triangle(v1, v0, v2);
        }
    }
    if (in.chimney && in.roof == RoofType::Gable) {
        const f32 cx = rng.Range(-hx * 0.6f, hx * 0.6f), cz = hz * 0.45f;
        walls.Box(Vec3(cx - 0.35f, height, cz - 0.35f), Vec3(cx + 0.35f, height + std::max(depth * 0.32f, 1.0f) * 0.7f + 1.2f, cz + 0.35f));
    }
    ProceduralMesh out;
    AddPrimitive(out, walls, Shading::Flat, UvMode::Given, Material(in.wall_color, 0.9f));
    AddPrimitive(out, roof, Shading::Flat, UvMode::Given, Material(in.roof_color, 0.85f));
    AddPrimitive(out, glass, Shading::Flat, UvMode::Given, Material(Vec3(0.55f, 0.7f, 0.85f), 0.1f));
    AddPrimitive(out, trim, Shading::Flat, UvMode::Given, Material(Vec3(0.32f, 0.22f, 0.14f), 0.8f));
    return out;
}

ProceduralMesh GenerateModular(const ModularParams& in, u32 seed) {
    Rng rng(Mix(seed, 0x4D4F4455u)); // "MODU"
    const f32 w = std::max(in.width, 0.2f), h = std::max(in.height, 0.2f), d = std::max(in.depth, 0.05f);
    MeshBuilder b;
    switch (in.kind) {
    case ModularKind::Wall: {
        if (in.window && w > 1.2f && h > 1.4f) {
            const f32 ww = w * 0.4f, wh = h * 0.4f, x0 = -ww * 0.5f, x1 = ww * 0.5f, y0 = h * 0.35f, y1 = y0 + wh;
            b.Box(Vec3(-w * 0.5f, 0, -d * 0.5f), Vec3(x0, h, d * 0.5f));
            b.Box(Vec3(x1, 0, -d * 0.5f), Vec3(w * 0.5f, h, d * 0.5f));
            b.Box(Vec3(x0, 0, -d * 0.5f), Vec3(x1, y0, d * 0.5f));
            b.Box(Vec3(x0, y1, -d * 0.5f), Vec3(x1, h, d * 0.5f));
        } else {
            b.Box(Vec3(-w * 0.5f, 0, -d * 0.5f), Vec3(w * 0.5f, h, d * 0.5f));
        }
        // A capping stone along the top, slightly proud.
        b.Box(Vec3(-w * 0.5f - 0.04f, h, -d * 0.5f - 0.04f), Vec3(w * 0.5f + 0.04f, h + 0.08f, d * 0.5f + 0.04f));
        break;
    }
    case ModularKind::Pillar: {
        const u32 sides = std::clamp(in.segments, 4u, 32u);
        const f32 r = w * 0.5f, base_h = std::min(h * 0.08f, 0.3f), cap_h = base_h;
        // A lathe: base plinth, shaft with a slight entasis, capital. Profile points (radius, y), joined into rings.
        const std::vector<std::pair<f32, f32>> profile = {{r * 1.25f, 0.0f}, {r * 1.25f, base_h}, {r * 0.92f, base_h}, {r * 0.85f, h * 0.5f},
                                                          {r * 0.78f, h - cap_h}, {r * 1.15f, h - cap_h}, {r * 1.15f, h}};
        std::vector<std::vector<u32>> rings;
        for (const auto& pr : profile) {
            std::vector<u32> ring;
            for (u32 k = 0; k <= sides; ++k) {
                const f32 a = static_cast<f32>(k % sides) / static_cast<f32>(sides) * 2.0f * kPi;
                ring.push_back(b.Vertex(Vec3(std::cos(a) * pr.first, pr.second, std::sin(a) * pr.first), static_cast<f32>(k) / static_cast<f32>(sides), pr.second));
            }
            rings.push_back(ring);
        }
        for (usize i = 0; i + 1 < rings.size(); ++i)
            for (u32 k = 0; k < sides; ++k) b.Quad(rings[i][k + 1], rings[i][k], rings[i + 1][k], rings[i + 1][k + 1]);
        const u32 bottom = b.Vertex(Vec3(0, 0, 0), 0.5f, 0.5f), top = b.Vertex(Vec3(0, h, 0), 0.5f, 0.5f);
        for (u32 k = 0; k < sides; ++k) {
            b.Triangle(bottom, rings.front()[k], rings.front()[k + 1]);
            b.Triangle(top, rings.back()[k + 1], rings.back()[k]);
        }
        break;
    }
    case ModularKind::Arch: {
        const f32 post = std::max(d * 1.5f, 0.3f), open = w, top_y = h;
        const f32 radius = open * 0.5f, spring_y = std::max(top_y - radius, 0.2f);
        b.Box(Vec3(-open * 0.5f - post, 0, -d * 0.5f), Vec3(-open * 0.5f, spring_y, d * 0.5f));
        b.Box(Vec3(open * 0.5f, 0, -d * 0.5f), Vec3(open * 0.5f + post, spring_y, d * 0.5f));
        // The curve: wedge voussoirs between an inner and an outer radius, each a box-like prism closed on every side.
        const u32 pieces = std::clamp(in.segments, 3u, 40u);
        for (u32 i = 0; i < pieces; ++i) {
            const f32 a0 = kPi * static_cast<f32>(i) / static_cast<f32>(pieces), a1 = kPi * static_cast<f32>(i + 1) / static_cast<f32>(pieces);
            // Angle runs from the right springer (0) over the top to the left (pi).
            const Vec3 in0(std::cos(a0) * radius, spring_y + std::sin(a0) * radius, 0), in1(std::cos(a1) * radius, spring_y + std::sin(a1) * radius, 0);
            const Vec3 ou0(std::cos(a0) * (radius + post), spring_y + std::sin(a0) * (radius + post), 0), ou1(std::cos(a1) * (radius + post), spring_y + std::sin(a1) * (radius + post), 0);
            const Vec3 fz(0, 0, d * 0.5f), bz(0, 0, -d * 0.5f);
            // Eight corners; faces wound counter-clockwise from outside. Going along increasing angle, with +z toward the viewer.
            const u32 f_in0 = b.Vertex(in0 + fz), f_in1 = b.Vertex(in1 + fz), f_ou0 = b.Vertex(ou0 + fz), f_ou1 = b.Vertex(ou1 + fz);
            const u32 b_in0 = b.Vertex(in0 + bz), b_in1 = b.Vertex(in1 + bz), b_ou0 = b.Vertex(ou0 + bz), b_ou1 = b.Vertex(ou1 + bz);
            b.Quad(f_in0, f_ou0, f_ou1, f_in1); // front, +z
            b.Quad(b_in1, b_ou1, b_ou0, b_in0); // back, -z
            b.Quad(f_ou0, b_ou0, b_ou1, f_ou1); // outer curve
            b.Quad(f_in1, b_in1, b_in0, f_in0); // inner curve (the soffit)
            b.Quad(f_in0, b_in0, b_ou0, f_ou0); // end at a0
            b.Quad(f_ou1, b_ou1, b_in1, f_in1); // end at a1
        }
        break;
    }
    case ModularKind::Stairs: {
        const u32 steps = std::clamp(in.segments, 1u, 64u);
        const f32 rise = h / static_cast<f32>(steps), run = d / static_cast<f32>(steps);
        for (u32 i = 0; i < steps; ++i) {
            // Each step is a block from the floor up, so the run is solid underneath.
            b.Box(Vec3(-w * 0.5f, 0, static_cast<f32>(i) * run - d * 0.5f), Vec3(w * 0.5f, rise * static_cast<f32>(i + 1), static_cast<f32>(i + 1) * run - d * 0.5f));
        }
        break;
    }
    }
    (void)rng;
    ProceduralMesh out;
    AddPrimitive(out, b, in.kind == ModularKind::Pillar || in.kind == ModularKind::Arch ? Shading::Smooth : Shading::Flat,
                 in.kind == ModularKind::Pillar ? UvMode::Given : UvMode::BoxProjected, Material(in.color, 0.9f));
    return out;
}

// ======================================================================================================================================
// Variants

const char* ProceduralKindName(ProceduralKind kind) {
    switch (kind) {
    case ProceduralKind::Rock: return "Rock";
    case ProceduralKind::Cliff: return "Cliff";
    case ProceduralKind::PineTree: return "PineTree";
    case ProceduralKind::BroadleafTree: return "BroadleafTree";
    case ProceduralKind::DeadTree: return "DeadTree";
    case ProceduralKind::House: return "House";
    case ProceduralKind::Tower: return "Tower";
    case ProceduralKind::Wall: return "Wall";
    case ProceduralKind::Pillar: return "Pillar";
    case ProceduralKind::Arch: return "Arch";
    case ProceduralKind::Stairs: return "Stairs";
    case ProceduralKind::Shrub: return "Shrub";
    case ProceduralKind::Grass: return "Grass";
    case ProceduralKind::Reeds: return "Reeds";
    case ProceduralKind::Cactus: return "Cactus";
    }
    return "Unknown";
}

ProceduralMesh GenerateVariant(ProceduralKind kind, u32 variant, u32 seed) {
    Rng rng(Mix(Mix(seed, static_cast<u32>(kind) + 1), variant));
    const u32 mesh_seed = rng.Next();
    switch (kind) {
    case ProceduralKind::Rock: {
        RockParams p;
        p.size = rng.Range(0.5f, 3.2f);
        p.elongation = rng.Range(0.0f, 0.8f);
        p.subdivisions = static_cast<u32>(rng.Int(2, 3));
        p.roughness = rng.Range(0.1f, 0.45f);
        p.cuts = static_cast<u32>(rng.Int(0, 9));
        p.flatten_bottom = rng.Range(0.2f, 0.7f);
        p.shading = rng.Chance(0.4f) ? Shading::Flat : Shading::Smooth;
        const f32 warm = rng.Range(-0.04f, 0.06f);
        p.color = Tint(Vec3(0.40f + warm, 0.38f, 0.36f - warm), rng, 0.25f);
        return GenerateRock(p, mesh_seed);
    }
    case ProceduralKind::Cliff: {
        CliffParams p;
        p.width = rng.Range(8.0f, 24.0f);
        p.height = rng.Range(5.0f, 18.0f);
        p.depth = rng.Range(2.0f, 5.0f);
        p.columns = static_cast<u32>(rng.Int(32, 56));
        p.rows = static_cast<u32>(rng.Int(24, 40));
        p.strata = static_cast<u32>(rng.Int(3, 12));
        p.roughness = rng.Range(0.2f, 0.6f);
        p.overhang = rng.Range(0.05f, 0.4f);
        p.color = Tint(Vec3(0.45f, 0.40f, 0.34f), rng, 0.2f);
        return GenerateCliff(p, mesh_seed);
    }
    case ProceduralKind::PineTree:
    case ProceduralKind::BroadleafTree:
    case ProceduralKind::DeadTree: {
        TreeParams p;
        p.species = kind == ProceduralKind::PineTree ? TreeSpecies::Pine : (kind == ProceduralKind::DeadTree ? TreeSpecies::Dead : TreeSpecies::Broadleaf);
        p.height = rng.Range(kind == ProceduralKind::DeadTree ? 4.0f : 5.0f, kind == ProceduralKind::PineTree ? 16.0f : 12.0f);
        p.trunk_radius = p.height * rng.Range(0.025f, 0.04f);
        p.sides = static_cast<u32>(rng.Int(6, 10));
        p.branch_levels = static_cast<u32>(rng.Int(2, 3));
        p.foliage = rng.Range(0.6f, 1.4f);
        p.lean = rng.Range(0.0f, 0.12f);
        p.foliage_detail = static_cast<u32>(rng.Int(0, 1));
        p.bark_color = Tint(Vec3(0.30f, 0.21f, 0.14f), rng, 0.25f);
        const f32 green = rng.Range(0.0f, 1.0f);
        p.leaf_color = Tint(Lerp(Vec3(0.18f, 0.38f, 0.14f), Vec3(0.40f, 0.52f, 0.14f), green), rng, 0.15f);
        if (kind == ProceduralKind::PineTree) p.leaf_color = Tint(Vec3(0.10f, 0.30f, 0.16f), rng, 0.2f);
        return GenerateTree(p, mesh_seed);
    }
    case ProceduralKind::House:
    case ProceduralKind::Tower: {
        BuildingParams p;
        const bool tower = kind == ProceduralKind::Tower;
        p.floors = static_cast<u32>(tower ? rng.Int(3, 6) : rng.Int(1, 3));
        p.bays_x = static_cast<u32>(tower ? rng.Int(1, 2) : rng.Int(2, 5));
        p.bays_z = static_cast<u32>(tower ? p.bays_x : rng.Int(2, 4));
        p.bay_size = rng.Range(2.6f, 3.6f);
        p.floor_height = rng.Range(2.7f, 3.4f);
        p.roof = tower || rng.Chance(0.2f) ? RoofType::Flat : RoofType::Gable;
        p.chimney = rng.Chance(0.6f);
        p.door = true;
        p.wall_color = Tint(Lerp(Vec3(0.78f, 0.72f, 0.62f), Vec3(0.62f, 0.58f, 0.55f), rng.Unit()), rng, 0.1f);
        p.roof_color = Tint(Lerp(Vec3(0.45f, 0.20f, 0.15f), Vec3(0.28f, 0.28f, 0.32f), rng.Unit()), rng, 0.15f);
        return GenerateBuilding(p, mesh_seed);
    }
    case ProceduralKind::Shrub: {
        ShrubParams p;
        p.height = rng.Range(0.6f, 1.7f);
        p.spread = rng.Range(0.8f, 1.5f);
        p.masses = static_cast<u32>(rng.Int(4, 10));
        p.detail = rng.Chance(0.4f) ? 1u : 0u;
        p.stems = static_cast<f32>(rng.Int(2, 6));
        p.leaf_color = Tint(Lerp(Vec3(0.20f, 0.40f, 0.14f), Vec3(0.38f, 0.48f, 0.16f), rng.Unit()), rng, 0.12f);
        return GenerateShrub(p, mesh_seed);
    }
    case ProceduralKind::Grass: {
        GrassParams p;
        p.height = rng.Range(0.3f, 0.8f);
        p.blades = static_cast<u32>(rng.Int(10, 22));
        p.width = rng.Range(0.03f, 0.06f);
        p.spread = rng.Range(0.12f, 0.25f);
        p.curl = rng.Range(0.15f, 0.5f);
        p.color = Tint(Lerp(Vec3(0.30f, 0.50f, 0.16f), Vec3(0.50f, 0.54f, 0.22f), rng.Unit()), rng, 0.1f);
        return GenerateGrass(p, mesh_seed);
    }
    case ProceduralKind::Reeds: {
        ReedParams p;
        p.height = rng.Range(1.0f, 2.3f);
        p.stems = static_cast<u32>(rng.Int(4, 10));
        p.spread = rng.Range(0.18f, 0.35f);
        p.heads = rng.Chance(0.75f);
        p.stem_color = Tint(Vec3(0.36f, 0.48f, 0.20f), rng, 0.15f);
        return GenerateReeds(p, mesh_seed);
    }
    case ProceduralKind::Cactus: {
        CactusParams p;
        p.height = rng.Range(1.2f, 4.5f);
        p.radius = p.height * rng.Range(0.05f, 0.085f);
        p.arms = static_cast<u32>(rng.Int(0, 3));
        p.sides = static_cast<u32>(rng.Int(6, 10));
        p.color = Tint(Vec3(0.22f, 0.40f, 0.20f), rng, 0.15f);
        return GenerateCactus(p, mesh_seed);
    }
    case ProceduralKind::Wall:
    case ProceduralKind::Pillar:
    case ProceduralKind::Arch:
    case ProceduralKind::Stairs: {
        ModularParams p;
        p.kind = kind == ProceduralKind::Wall ? ModularKind::Wall : (kind == ProceduralKind::Pillar ? ModularKind::Pillar : (kind == ProceduralKind::Arch ? ModularKind::Arch : ModularKind::Stairs));
        p.color = Tint(Vec3(0.62f, 0.60f, 0.56f), rng, 0.15f);
        switch (p.kind) {
        case ModularKind::Wall: p.width = 2.0f * static_cast<f32>(rng.Int(1, 4)); p.height = rng.Range(2.4f, 4.0f); p.depth = rng.Range(0.3f, 0.6f); p.window = rng.Chance(0.4f); break;
        case ModularKind::Pillar: p.width = rng.Range(0.5f, 1.1f); p.height = rng.Range(2.5f, 6.0f); p.segments = static_cast<u32>(rng.Int(6, 16)); break;
        case ModularKind::Arch: p.width = rng.Range(2.0f, 4.0f); p.height = rng.Range(3.5f, 6.0f); p.depth = rng.Range(0.4f, 0.8f); p.segments = static_cast<u32>(rng.Int(6, 14)); break;
        case ModularKind::Stairs: p.width = rng.Range(1.5f, 3.5f); p.height = rng.Range(1.0f, 3.0f); p.depth = rng.Range(2.0f, 5.0f); p.segments = static_cast<u32>(rng.Int(4, 14)); break;
        }
        return GenerateModular(p, mesh_seed);
    }
    }
    return {};
}

// ======================================================================================================================================
// Checks

MeshStats ComputeStats(const MeshData& mesh) {
    MeshStats s;
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    std::map<std::pair<std::array<i64, 3>, std::array<i64, 3>>, i32> edges;
    auto key = [](const f32* p) { return std::array<i64, 3>{static_cast<i64>(std::llround(p[0] * 1000.0f)), static_cast<i64>(std::llround(p[1] * 1000.0f)), static_cast<i64>(std::llround(p[2] * 1000.0f))}; };
    f64 volume = 0.0;
    for (const MeshPrimitiveData& prim : mesh.primitives) {
        s.vertices += static_cast<u32>(prim.vertices.size());
        s.triangles += static_cast<u32>(prim.indices.size() / 3);
        for (const MeshVertex& v : prim.vertices) {
            lo = Vec3(std::min(lo.x, v.position[0]), std::min(lo.y, v.position[1]), std::min(lo.z, v.position[2]));
            hi = Vec3(std::max(hi.x, v.position[0]), std::max(hi.y, v.position[1]), std::max(hi.z, v.position[2]));
        }
        for (usize t = 0; t + 2 < prim.indices.size(); t += 3) {
            const u32 idx[3] = {prim.indices[t], prim.indices[t + 1], prim.indices[t + 2]};
            if (idx[0] >= prim.vertices.size() || idx[1] >= prim.vertices.size() || idx[2] >= prim.vertices.size()) continue;
            const f32 *a = prim.vertices[idx[0]].position, *b = prim.vertices[idx[1]].position, *c = prim.vertices[idx[2]].position;
            volume += (static_cast<f64>(a[0]) * (static_cast<f64>(b[1]) * c[2] - static_cast<f64>(b[2]) * c[1]) - static_cast<f64>(a[1]) * (static_cast<f64>(b[0]) * c[2] - static_cast<f64>(b[2]) * c[0]) +
                       static_cast<f64>(a[2]) * (static_cast<f64>(b[0]) * c[1] - static_cast<f64>(b[1]) * c[0])) / 6.0;
            for (int e = 0; e < 3; ++e) {
                const auto p = key(prim.vertices[idx[e]].position), q = key(prim.vertices[idx[(e + 1) % 3]].position);
                if (p == q) continue;
                ++edges[p < q ? std::make_pair(p, q) : std::make_pair(q, p)];
            }
        }
    }
    if (s.vertices > 0) { s.bounds_min = lo; s.bounds_max = hi; }
    s.volume = static_cast<f32>(volume);
    for (const auto& [e, count] : edges) if (count == 1) ++s.open_edges;
    return s;
}

bool ValidateMesh(const ProceduralMesh& mesh, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    for (usize pi = 0; pi < mesh.mesh.primitives.size(); ++pi) {
        const MeshPrimitiveData& p = mesh.mesh.primitives[pi];
        const std::string where = "primitive " + std::to_string(pi) + ": ";
        if (p.indices.size() % 3 != 0) return fail(where + "index count is not a multiple of three");
        if (p.material < 0 || static_cast<usize>(p.material) >= mesh.materials.size()) return fail(where + "material index out of range");
        for (const u32 i : p.indices) if (i >= p.vertices.size()) return fail(where + "index out of range");
        for (usize vi = 0; vi < p.vertices.size(); ++vi) {
            const MeshVertex& v = p.vertices[vi];
            for (int k = 0; k < 3; ++k) if (!std::isfinite(v.position[k]) || !std::isfinite(v.normal[k]) || !std::isfinite(v.tangent[k])) return fail(where + "non-finite value");
            if (!std::isfinite(v.uv[0]) || !std::isfinite(v.uv[1]) || !std::isfinite(v.tangent[3])) return fail(where + "non-finite value");
            const f32 n = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
            if (std::fabs(n - 1.0f) > 1e-3f) return fail(where + "normal is not unit length");
            const f32 tl = std::sqrt(v.tangent[0] * v.tangent[0] + v.tangent[1] * v.tangent[1] + v.tangent[2] * v.tangent[2]);
            if (std::fabs(tl - 1.0f) > 1e-3f || std::fabs(std::fabs(v.tangent[3]) - 1.0f) > 1e-6f) return fail(where + "tangent is not a unit vector with w = +-1");
        }
    }
    return true;
}

// ======================================================================================================================================
// GLB

namespace {

void AppendBytes(std::vector<u8>& out, const void* data, usize size) {
    const u8* p = static_cast<const u8*>(data);
    out.insert(out.end(), p, p + size);
}
void Pad4(std::vector<u8>& out, u8 fill) { while (out.size() % 4 != 0) out.push_back(fill); }
std::string Num(f32 v) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(v));
    return buffer;
}

} // namespace

std::vector<u8> EncodeGlb(const ProceduralMesh& mesh) {
    std::vector<u8> bin;
    std::string views, accessors, primitives, materials;
    auto add_view = [&](const void* data, usize size, int target) {
        Pad4(bin, 0);
        const usize offset = bin.size();
        AppendBytes(bin, data, size);
        if (!views.empty()) views += ",";
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(size) + ",\"target\":" + std::to_string(target) + "}";
        return static_cast<int>(std::count(views.begin(), views.end(), '{')) - 1;
    };
    int accessor_count = 0;
    auto add_accessor = [&](int view, int component_type, usize count, const char* type, const std::string& extra = "") {
        if (accessor_count > 0) accessors += ",";
        accessors += "{\"bufferView\":" + std::to_string(view) + ",\"componentType\":" + std::to_string(component_type) + ",\"count\":" + std::to_string(count) +
                     ",\"type\":\"" + type + "\"" + extra + "}";
        return accessor_count++;
    };
    for (usize pi = 0; pi < mesh.mesh.primitives.size(); ++pi) {
        const MeshPrimitiveData& p = mesh.mesh.primitives[pi];
        if (p.vertices.empty() || p.indices.empty()) continue;
        std::vector<f32> pos, nor, uv, tan;
        pos.reserve(p.vertices.size() * 3);
        for (const MeshVertex& v : p.vertices) {
            pos.insert(pos.end(), v.position, v.position + 3);
            nor.insert(nor.end(), v.normal, v.normal + 3);
            uv.insert(uv.end(), v.uv, v.uv + 2);
            tan.insert(tan.end(), v.tangent, v.tangent + 4);
        }
        const std::string bounds = ",\"min\":[" + Num(p.bounds_min.x) + "," + Num(p.bounds_min.y) + "," + Num(p.bounds_min.z) + "],\"max\":[" + Num(p.bounds_max.x) + "," +
                                   Num(p.bounds_max.y) + "," + Num(p.bounds_max.z) + "]";
        const int a_pos = add_accessor(add_view(pos.data(), pos.size() * 4, 34962), 5126, p.vertices.size(), "VEC3", bounds);
        const int a_nor = add_accessor(add_view(nor.data(), nor.size() * 4, 34962), 5126, p.vertices.size(), "VEC3");
        const int a_uv = add_accessor(add_view(uv.data(), uv.size() * 4, 34962), 5126, p.vertices.size(), "VEC2");
        const int a_tan = add_accessor(add_view(tan.data(), tan.size() * 4, 34962), 5126, p.vertices.size(), "VEC4");
        const int a_idx = add_accessor(add_view(p.indices.data(), p.indices.size() * 4, 34963), 5125, p.indices.size(), "SCALAR");
        if (!primitives.empty()) primitives += ",";
        primitives += "{\"attributes\":{\"POSITION\":" + std::to_string(a_pos) + ",\"NORMAL\":" + std::to_string(a_nor) + ",\"TEXCOORD_0\":" + std::to_string(a_uv) +
                      ",\"TANGENT\":" + std::to_string(a_tan) + "},\"indices\":" + std::to_string(a_idx) + ",\"material\":" + std::to_string(std::max(p.material, 0)) + "}";
    }
    // Textures: each distinct base colour image becomes an image, a texture on it, and the material's baseColorTexture. The uri is the path as given
    // (relative to the .glb), which is how the player finds it in the package.
    std::vector<std::string> images;
    bool any_water = false, any_wind = false;
    for (usize mi = 0; mi < mesh.materials.size(); ++mi) {
        const MaterialData& m = mesh.materials[mi];
        if (mi > 0) materials += ",";
        auto image_index = [&](const std::string& path) {
            usize index = 0;
            while (index < images.size() && images[index] != path) ++index;
            if (index == images.size()) images.push_back(path);
            return index;
        };
        std::string texture, normal, alpha;
        if (!m.base_color_texture.empty()) texture = ",\"baseColorTexture\":{\"index\":" + std::to_string(image_index(m.base_color_texture)) + "}";
        if (!m.normal_texture.empty()) normal = ",\"normalTexture\":{\"index\":" + std::to_string(image_index(m.normal_texture)) + ",\"scale\":" + Num(m.normal_scale) + "}";
        if (m.alpha_mode == MaterialAlphaMode::Blend) alpha = ",\"alphaMode\":\"BLEND\"";
        else if (m.alpha_mode == MaterialAlphaMode::Mask) alpha = ",\"alphaMode\":\"MASK\",\"alphaCutoff\":" + Num(m.alpha_cutoff);
        std::string extensions_body;
        if (m.water) { any_water = true; extensions_body += "\"AETHER_materials_water\":{}"; }
        if (m.wind > 0.0f) { any_wind = true; extensions_body += std::string(extensions_body.empty() ? "" : ",") + "\"AETHER_materials_wind\":{\"strength\":" + Num(m.wind) + "}"; }
        const std::string extensions = extensions_body.empty() ? "" : ",\"extensions\":{" + extensions_body + "}";
        materials += "{\"name\":\"material" + std::to_string(mi) + "\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[" + Num(m.base_color.x) + "," + Num(m.base_color.y) + "," +
                     Num(m.base_color.z) + "," + Num(m.base_color.w) + "],\"metallicFactor\":" + Num(m.metallic) + ",\"roughnessFactor\":" + Num(m.roughness) + texture + "}" + normal + alpha + extensions + "}";
    }
    std::string image_json, texture_json;
    for (usize i = 0; i < images.size(); ++i) {
        std::string escaped;
        for (const char c : images[i]) { if (c == '"' || c == '\\') escaped += '\\'; escaped += c; }
        image_json += (i ? "," : "") + std::string("{\"uri\":\"") + escaped + "\"}";
        texture_json += (i ? "," : "") + std::string("{\"source\":") + std::to_string(i) + "}";
    }
    Pad4(bin, 0);
    std::string json = std::string("{\"asset\":{\"version\":\"2.0\",\"generator\":\"Aether procedural mesh\"},") + (any_water || any_wind ? std::string("\"extensionsUsed\":[") + (any_water ? "\"AETHER_materials_water\"" : "") + (any_water && any_wind ? "," : "") + (any_wind ? "\"AETHER_materials_wind\"" : "") + "]," : std::string()) + "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
                       "\"meshes\":[{\"primitives\":[" + primitives + "]}],";
    if (!materials.empty()) json += "\"materials\":[" + materials + "],";
    if (!images.empty()) json += "\"images\":[" + image_json + "],\"textures\":[" + texture_json + "],";
    json += "\"accessors\":[" + accessors + "],\"bufferViews\":[" + views + "],\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]}";
    while (json.size() % 4 != 0) json.push_back(' ');
    std::vector<u8> out;
    const u32 total = static_cast<u32>(12 + 8 + json.size() + 8 + bin.size());
    const u32 magic = 0x46546C67u, version = 2u, json_type = 0x4E4F534Au, bin_type = 0x004E4942u, json_len = static_cast<u32>(json.size()), bin_len = static_cast<u32>(bin.size());
    AppendBytes(out, &magic, 4); AppendBytes(out, &version, 4); AppendBytes(out, &total, 4);
    AppendBytes(out, &json_len, 4); AppendBytes(out, &json_type, 4); AppendBytes(out, json.data(), json.size());
    AppendBytes(out, &bin_len, 4); AppendBytes(out, &bin_type, 4); AppendBytes(out, bin.data(), bin.size());
    return out;
}

} // namespace aether::assets::procedural
