#include "vegetation_batch.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace aether::player {

namespace {

constexpr f32 kPi = 3.14159265f;

// Palette columns. Three consecutive shades of a colour let each plant pick its own (the tint that stops a field reading as a pattern).
enum Col : u32 {
    kBark = 0, kDarkBark = 1, kBirchBark = 2,
    kPine = 3,       // 3..5
    kBirchLeaf = 6,  // 6..8
    kShrub = 9,      // 9..11
    kGrass = 12,     // 12..14
    kRock = 15,      // 15..17
    kReed = 18,      // 18..20
};

u32 Hash(u32 a, u32 b = 0, u32 c = 0) {
    u32 h = a * 374761393u + b * 668265263u + c * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
f32 Hash01(u32 a, u32 b = 0, u32 c = 0) { return static_cast<f32>(Hash(a, b, c) & 0xffffffu) / 16777215.0f; }

class Builder {
public:
    // `sway` scales the wind weight of what is added next (0: rigid).
    u32 Add(const Vec3& p, const Vec3& n, u32 column, u32 span, f32 sway) {
        VegVertex v{};
        v.position[0] = p.x, v.position[1] = p.y, v.position[2] = p.z;
        const Vec3 nn = n.Length() > 1e-6f ? n.Normalized() : Vec3(0, 1, 0);
        v.normal[0] = nn.x, v.normal[1] = nn.y, v.normal[2] = nn.z;
        v.uv[0] = (static_cast<f32>(column) + 0.5f) / static_cast<f32>(kPaletteColumns);
        v.uv[1] = sway; // finished by Finish(): the wind weight is this part's share of the height
        mesh.vertices.push_back(v);
        mesh.palette_span.push_back(static_cast<u8>(span));
        return static_cast<u32>(mesh.vertices.size() - 1);
    }
    void Tri(u32 a, u32 b, u32 c) { mesh.indices.insert(mesh.indices.end(), {a, b, c}); }

    // A cone (or a cylinder when the radii match) of `sides`, from y0 (radius r0) to y1 (radius r1).
    void Frustum(f32 y0, f32 y1, f32 r0, f32 r1, u32 sides, u32 column, u32 span, f32 sway, f32 x = 0.0f, f32 z = 0.0f) {
        const f32 slope = (r0 - r1) / std::max(y1 - y0, 1e-3f);
        std::vector<u32> low, high;
        for (u32 i = 0; i <= sides; ++i) {
            const f32 a = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(sides);
            const Vec3 radial(std::cos(a), 0, std::sin(a));
            const Vec3 normal = radial + Vec3(0, slope, 0);
            low.push_back(Add(Vec3(x + radial.x * r0, y0, z + radial.z * r0), normal, column, span, sway));
            high.push_back(Add(Vec3(x + radial.x * r1, y1, z + radial.z * r1), normal, column, span, sway));
        }
        for (u32 i = 0; i < sides; ++i) {
            Tri(low[i], high[i], low[i + 1]);
            Tri(low[i + 1], high[i], high[i + 1]);
        }
    }

    // A blob: an ellipsoid of `rings` x `sectors`.
    void Blob(const Vec3& c, const Vec3& radii, u32 rings, u32 sectors, u32 column, u32 span, f32 sway) {
        std::vector<std::vector<u32>> grid;
        for (u32 r = 0; r <= rings; ++r) {
            const f32 phi = kPi * static_cast<f32>(r) / static_cast<f32>(rings);
            std::vector<u32> row;
            for (u32 s = 0; s <= sectors; ++s) {
                const f32 theta = 2.0f * kPi * static_cast<f32>(s) / static_cast<f32>(sectors);
                const Vec3 d(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
                row.push_back(Add(Vec3(c.x + d.x * radii.x, c.y + d.y * radii.y, c.z + d.z * radii.z), Vec3(d.x / radii.x, d.y / radii.y, d.z / radii.z), column,
                                  span, sway));
            }
            grid.push_back(std::move(row));
        }
        for (u32 r = 0; r < rings; ++r) {
            for (u32 s = 0; s < sectors; ++s) {
                Tri(grid[r][s], grid[r + 1][s], grid[r][s + 1]);
                Tri(grid[r][s + 1], grid[r + 1][s], grid[r + 1][s + 1]);
            }
        }
    }

    // One blade: a triangle from a base `width` wide to a tip `height` up, leaning `lean` metres along `angle`. Its normal leans toward up, so
    // the two faces of a two-sided blade light alike.
    void Blade(f32 x, f32 z, f32 angle, f32 width, f32 height, f32 lean, u32 column, u32 span) {
        const Vec3 side(-std::sin(angle), 0, std::cos(angle)), out(std::cos(angle), 0, std::sin(angle));
        const Vec3 n = out * 0.35f + Vec3(0, 1, 0);
        const u32 a = Add(Vec3(x - side.x * width * 0.5f, 0, z - side.z * width * 0.5f), n, column, span, 0.0f);
        const u32 b = Add(Vec3(x + side.x * width * 0.5f, 0, z + side.z * width * 0.5f), n, column, span, 0.0f);
        const u32 t = Add(Vec3(x + out.x * lean, height, z + out.z * lean), n, column, span, 1.0f);
        Tri(a, b, t);
    }

    // A faceted rock: an icosahedron with its corners pushed about by `seed`, flat shaded, squashed to `radii`.
    void Rock(const Vec3& radii, u32 seed, u32 column, u32 span) {
        const f32 t = (1.0f + std::sqrt(5.0f)) * 0.5f;
        std::array<Vec3, 12> v = {Vec3(-1, t, 0), Vec3(1, t, 0), Vec3(-1, -t, 0), Vec3(1, -t, 0), Vec3(0, -1, t), Vec3(0, 1, t),
                                  Vec3(0, -1, -t), Vec3(0, 1, -t), Vec3(t, 0, -1), Vec3(t, 0, 1), Vec3(-t, 0, -1), Vec3(-t, 0, 1)};
        for (usize i = 0; i < v.size(); ++i) {
            v[i] = v[i].Normalized() * (0.78f + 0.3f * Hash01(seed, static_cast<u32>(i)));
            v[i] = Vec3(v[i].x * radii.x, v[i].y * radii.y, v[i].z * radii.z);
        }
        static const u32 f[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                     {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
        const f32 lift = radii.y * 0.45f; // sunk into the ground a little, so it sits in it
        for (const auto& tri : f) {
            const Vec3 a = v[tri[0]], b = v[tri[1]], c = v[tri[2]];
            Vec3 n(0, 1, 0);
            const Vec3 e1 = b - a, e2 = c - a;
            n = Vec3(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
            const Vec3 centre = (a + b + c) * (1.0f / 3.0f);
            if (n.x * centre.x + n.y * centre.y + n.z * centre.z < 0) n = Vec3(-n.x, -n.y, -n.z);
            const u32 ia = Add(Vec3(a.x, a.y + lift, a.z), n, column, span, 0.0f), ib = Add(Vec3(b.x, b.y + lift, b.z), n, column, span, 0.0f),
                      ic = Add(Vec3(c.x, c.y + lift, c.z), n, column, span, 0.0f);
            Tri(ia, ib, ic);
        }
    }

    // Wind weights: what Add was given as a 0..1 flag becomes that flag times the vertex's share of `height` (squared in the shader), scaled by `reach`.
    PlantMesh Finish(f32 height, f32 radius, f32 reach) {
        for (VegVertex& v : mesh.vertices) {
            const f32 share = std::clamp(v.position[1] / std::max(height, 1e-3f), 0.0f, 1.0f);
            v.uv[1] = v.uv[1] > 0.0f ? std::min(1.0f, share * reach) : 0.0f;
        }
        mesh.height = height;
        mesh.radius = radius;
        return std::move(mesh);
    }

    PlantMesh mesh;
};

PlantMesh MakePine() {
    Builder b;
    b.Frustum(0.0f, 7.0f, 0.28f, 0.1f, 6, kBark, 0, 0.0f);
    // Four tiers of needles, each wider below and darker at the foot; the tiers overlap so the crown reads as one.
    const f32 y0[4] = {1.6f, 3.1f, 4.6f, 6.0f}, h[4] = {3.2f, 2.9f, 2.6f, 2.5f}, r[4] = {2.1f, 1.7f, 1.3f, 0.9f};
    for (int i = 0; i < 4; ++i) b.Frustum(y0[i], y0[i] + h[i], r[i], 0.04f, 8, kPine, 2, 1.0f);
    return b.Finish(8.5f, 2.1f, 0.5f);
}

PlantMesh MakeBirch() {
    Builder b;
    b.Frustum(0.0f, 5.2f, 0.17f, 0.08f, 6, kBirchBark, 0, 0.0f);
    b.Blob(Vec3(0.0f, 5.6f, 0.0f), Vec3(1.7f, 1.5f, 1.7f), 4, 7, kBirchLeaf, 2, 1.0f);
    b.Blob(Vec3(0.9f, 4.5f, 0.4f), Vec3(1.2f, 1.0f, 1.2f), 3, 6, kBirchLeaf, 2, 1.0f);
    b.Blob(Vec3(-0.8f, 4.9f, -0.5f), Vec3(1.1f, 1.0f, 1.1f), 3, 6, kBirchLeaf, 2, 1.0f);
    return b.Finish(7.2f, 2.4f, 0.5f);
}

PlantMesh MakeShrub() {
    Builder b;
    b.Blob(Vec3(0.0f, 0.55f, 0.0f), Vec3(0.85f, 0.6f, 0.85f), 4, 7, kShrub, 2, 1.0f);
    b.Blob(Vec3(0.45f, 0.4f, 0.3f), Vec3(0.55f, 0.42f, 0.55f), 3, 6, kShrub, 2, 1.0f);
    return b.Finish(1.15f, 1.0f, 0.5f);
}

PlantMesh MakeGrass() {
    Builder b;
    // A tuft of seven blades fanned around the middle, each its own height and lean.
    for (u32 i = 0; i < 7; ++i) {
        const f32 a = 2.0f * kPi * static_cast<f32>(i) / 7.0f + 0.4f * Hash01(11, i);
        const f32 off = 0.05f + 0.07f * Hash01(12, i);
        b.Blade(std::cos(a) * off, std::sin(a) * off, a + 1.0f, 0.07f, 0.35f + 0.25f * Hash01(13, i), 0.08f + 0.12f * Hash01(14, i), kGrass, 2);
    }
    return b.Finish(0.6f, 0.3f, 1.0f);
}

PlantMesh MakeBoulder() {
    Builder b;
    b.Rock(Vec3(0.9f, 0.65f, 0.8f), 5, kRock, 2);
    return b.Finish(1.2f, 1.0f, 0.0f);
}

PlantMesh MakeReeds() {
    Builder b;
    for (u32 i = 0; i < 9; ++i) {
        const f32 a = 2.0f * kPi * static_cast<f32>(i) / 9.0f + 0.3f * Hash01(21, i);
        const f32 off = 0.04f + 0.1f * Hash01(22, i);
        b.Blade(std::cos(a) * off, std::sin(a) * off, a + 0.6f, 0.05f, 0.9f + 0.6f * Hash01(23, i), 0.1f + 0.15f * Hash01(24, i), kReed, 2);
    }
    return b.Finish(1.5f, 0.3f, 1.0f);
}

Vec3 Rotate(const Quaternion& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const Vec3 t = Vec3(u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x) * 2.0f;
    const Vec3 ut(u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x);
    return v + t * q.w + ut;
}

} // namespace

const char* PlantKindName(PlantKind kind) {
    switch (kind) {
    case PlantKind::Pine: return "Pine";
    case PlantKind::Birch: return "Birch";
    case PlantKind::Shrub: return "Shrub";
    case PlantKind::Grass: return "Grass";
    case PlantKind::Boulder: return "Boulder";
    case PlantKind::Reeds: return "Reeds";
    }
    return "?";
}

std::array<u8, kPaletteColumns * 4> PalettePixels() {
    const u8 rgb[kPaletteColumns][3] = {
        {92, 68, 48}, {70, 52, 38}, {222, 220, 210},                      // bark, dark bark, birch bark
        {46, 98, 56}, {58, 114, 64}, {72, 130, 72},                        // pine
        {112, 146, 56}, {130, 162, 60}, {150, 176, 70},                   // birch leaf
        {66, 106, 50}, {82, 122, 54}, {98, 138, 60},                      // shrub
        {104, 146, 58}, {124, 164, 64}, {146, 180, 74},                   // grass
        {108, 104, 98}, {128, 124, 118}, {94, 92, 90},                    // rock
        {146, 148, 78}, {166, 166, 90}, {130, 138, 70},                   // reeds
        {255, 0, 255}, {255, 0, 255}, {255, 0, 255},                      // spare: magenta, so a wrong column shows
    };
    std::array<u8, kPaletteColumns * 4> out{};
    for (u32 i = 0; i < kPaletteColumns; ++i) {
        out[i * 4 + 0] = rgb[i][0], out[i * 4 + 1] = rgb[i][1], out[i * 4 + 2] = rgb[i][2], out[i * 4 + 3] = 255;
    }
    return out;
}

const PlantMesh& PlantPrototype(PlantKind kind) {
    static const std::array<PlantMesh, kPlantKindCount> meshes = {MakePine(), MakeBirch(), MakeShrub(), MakeGrass(), MakeBoulder(), MakeReeds()};
    return meshes[static_cast<usize>(kind)];
}

VegetationRules MakeVegetationRules(const ProceduralTerrain& terrain) {
    const f32 density = std::clamp(terrain.vegetation_density, 0.0f, 4.0f);
    const auto budget = [&](u32 total, f32 share) { return static_cast<u32>(static_cast<f32>(total) * share * density); };
    VegetationRules out;
    for (terrain::ScatterRule rule : terrain::DefaultScatterRules()) {
        PlantKind kind;
        if (rule.name == "Pine") { kind = PlantKind::Pine; rule.max_instances = budget(1500, 0.7f); }
        else if (rule.name == "Birch") { kind = PlantKind::Birch; rule.max_instances = budget(1500, 0.3f); }
        else if (rule.name == "Grass") { kind = PlantKind::Grass; rule.max_instances = budget(30000, 0.8f); }
        else if (rule.name == "Shrub") { kind = PlantKind::Shrub; rule.max_instances = budget(30000, 0.15f); }
        else if (rule.name == "Reeds") { kind = PlantKind::Reeds; rule.max_instances = budget(30000, 0.05f); }
        else if (rule.name == "Boulder") {
            kind = PlantKind::Boulder;
            rule.max_instances = budget(1000, 1.0f);
            rule.density = 2.0f; // the default 0.35 gives a few dozen boulders in a 512 m valley, a tenth of the budget
        } else continue; // the desert and snow rules do not belong in a temperate valley
        out.rules.push_back(std::move(rule));
        out.kinds.push_back(kind);
    }
    return out;
}

VegetationBatch BatchVegetation(std::span<const terrain::ScatterInstance> instances, std::span<const PlantKind> kinds, f32 chunk_size) {
    VegetationBatch batch;
    chunk_size = std::max(chunk_size, 1.0f);
    std::map<std::tuple<i32, i32, u8>, usize> index; // (chunk x, chunk z, kind) -> chunks[]
    for (const terrain::ScatterInstance& inst : instances) {
        if (inst.rule >= kinds.size()) continue;
        const PlantKind kind = kinds[inst.rule];
        const PlantMesh& proto = PlantPrototype(kind);
        const i32 cx = static_cast<i32>(std::floor(inst.position.x / chunk_size)), cz = static_cast<i32>(std::floor(inst.position.z / chunk_size));
        const auto key = std::make_tuple(cx, cz, static_cast<u8>(kind));
        auto it = index.find(key);
        if (it == index.end()) {
            VegChunk chunk;
            chunk.chunk_x = cx, chunk.chunk_z = cz, chunk.kind = kind;
            chunk.bounds_min = Vec3(1e30f, 1e30f, 1e30f);
            chunk.bounds_max = Vec3(-1e30f, -1e30f, -1e30f);
            batch.chunks.push_back(std::move(chunk));
            it = index.emplace(key, batch.chunks.size() - 1).first;
        }
        VegChunk& chunk = batch.chunks[it->second];
        const u32 base = static_cast<u32>(chunk.vertices.size());
        // The shade this plant takes: from where it stands, so the same terrain gives the same plants every run.
        const u32 variant_hash = Hash(static_cast<u32>(std::lround(inst.position.x * 8.0f)), static_cast<u32>(std::lround(inst.position.z * 8.0f)), 99);
        const Quaternion q = inst.rotation.Normalized();
        for (usize i = 0; i < proto.vertices.size(); ++i) {
            VegVertex v = proto.vertices[i];
            const Vec3 p = Rotate(q, Vec3(v.position[0], v.position[1], v.position[2]) * inst.scale) + inst.position;
            const Vec3 n = Rotate(q, Vec3(v.normal[0], v.normal[1], v.normal[2]));
            v.position[0] = p.x, v.position[1] = p.y, v.position[2] = p.z;
            v.normal[0] = n.x, v.normal[1] = n.y, v.normal[2] = n.z;
            const u32 span = proto.palette_span[i];
            if (span > 0) v.uv[0] += static_cast<f32>(variant_hash % (span + 1)) / static_cast<f32>(kPaletteColumns);
            chunk.vertices.push_back(v);
            chunk.bounds_min = Vec3(std::min(chunk.bounds_min.x, p.x), std::min(chunk.bounds_min.y, p.y), std::min(chunk.bounds_min.z, p.z));
            chunk.bounds_max = Vec3(std::max(chunk.bounds_max.x, p.x), std::max(chunk.bounds_max.y, p.y), std::max(chunk.bounds_max.z, p.z));
        }
        for (const u32 idx : proto.indices) chunk.indices.push_back(base + idx);
        ++chunk.instances;
        ++batch.instances;
        ++batch.instances_by_kind[static_cast<usize>(kind)];
    }
    // A stable order whatever the scatter's: by chunk, then kind.
    std::sort(batch.chunks.begin(), batch.chunks.end(), [](const VegChunk& a, const VegChunk& b) {
        return std::tie(a.chunk_z, a.chunk_x, a.kind) < std::tie(b.chunk_z, b.chunk_x, b.kind);
    });
    for (const VegChunk& c : batch.chunks) {
        batch.vertices += c.vertices.size();
        batch.triangles += c.indices.size() / 3;
    }
    return batch;
}

} // namespace aether::player
