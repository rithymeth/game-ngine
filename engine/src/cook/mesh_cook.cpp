#include "aether/cook/mesh_cook.h"

#include "aether/pak/pak.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace aether::cook {

namespace {

constexpr char kMagic[4] = {'A', 'M', 'S', 'C'};
constexpr u32 kVersion = 3;
// v1: primitives only. v2: a trailing HLOD proxy table (Phase 42.4); a v1 file
// loads with no proxies. v3: a trailing table of per-primitive cluster hierarchies; a v1 or v2 file loads with none.

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

class Writer {
public:
    void U32(u32 v) {
        for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void I32(i32 v) { U32(static_cast<u32>(v)); }
    void F32(f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, 4);
        U32(bits);
    }
    void Raw(const void* data, usize n) {
        const u8* p = static_cast<const u8*>(data);
        bytes_.insert(bytes_.end(), p, p + n);
    }
    std::vector<u8>& bytes() { return bytes_; }

private:
    std::vector<u8> bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const u8> bytes) : bytes_(bytes) {}
    usize Remaining() const { return bytes_.size() - pos_; }
    bool U32(u32& v) {
        if (Remaining() < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(bytes_[pos_ + i]) << (8 * i);
        pos_ += 4;
        return true;
    }
    bool I32(i32& v) {
        u32 u;
        if (!U32(u)) return false;
        v = static_cast<i32>(u);
        return true;
    }
    bool F32(f32& v) {
        u32 bits;
        if (!U32(bits)) return false;
        std::memcpy(&v, &bits, 4);
        return true;
    }
    bool Raw(void* out, usize n) {
        if (Remaining() < n) return false;
        if (n) std::memcpy(out, bytes_.data() + pos_, n);
        pos_ += n;
        return true;
    }
    // True when `count` items of `item_bytes` still fit, without overflowing.
    bool Fits(u32 count, usize item_bytes) const { return item_bytes == 0 || count <= Remaining() / item_bytes; }

private:
    std::span<const u8> bytes_;
    usize pos_ = 0;
};

void WriteClusters(Writer& w, const ClusterHierarchy& h) {
    w.U32(h.level_count);
    w.U32(h.leaf_count);
    w.F32(h.mesh_extent);
    w.U32(static_cast<u32>(h.indices.size()));
    for (u32 i : h.indices) w.U32(i);
    w.U32(static_cast<u32>(h.clusters.size()));
    for (const Cluster& c : h.clusters) {
        w.U32(c.index_offset); w.U32(c.triangle_count); w.U32(c.level);
        w.F32(c.center.x); w.F32(c.center.y); w.F32(c.center.z); w.F32(c.radius); w.F32(c.error);
        w.F32(c.parent_center.x); w.F32(c.parent_center.y); w.F32(c.parent_center.z); w.F32(c.parent_radius); w.F32(c.parent_error);
        w.F32(c.bounds.center.x); w.F32(c.bounds.center.y); w.F32(c.bounds.center.z); w.F32(c.bounds.radius);
        w.F32(c.bounds.cone_axis.x); w.F32(c.bounds.cone_axis.y); w.F32(c.bounds.cone_axis.z); w.F32(c.bounds.cone_cutoff);
        w.I32(c.group); w.I32(c.source_group);
    }
    w.U32(static_cast<u32>(h.groups.size()));
    for (const ClusterGroup& g : h.groups) {
        w.U32(static_cast<u32>(g.inputs.size()));
        for (u32 i : g.inputs) w.U32(i);
        w.U32(static_cast<u32>(g.outputs.size()));
        for (u32 i : g.outputs) w.U32(i);
        w.F32(g.center.x); w.F32(g.center.y); w.F32(g.center.z); w.F32(g.radius); w.F32(g.error);
    }
}

void WriteMeshlets(Writer& w, const MeshletMesh& m) {
    w.U32(static_cast<u32>(m.meshlets.size()));
    for (const Meshlet& ml : m.meshlets) {
        w.U32(ml.vertex_offset);
        w.U32(ml.triangle_offset);
        w.U32(ml.vertex_count);
        w.U32(ml.triangle_count);
    }
    w.U32(static_cast<u32>(m.vertices.size()));
    for (u32 v : m.vertices) w.U32(v);
    w.U32(static_cast<u32>(m.triangles.size()));
    w.Raw(m.triangles.data(), m.triangles.size());
    for (const MeshletBounds& b : m.bounds) {
        w.F32(b.center.x); w.F32(b.center.y); w.F32(b.center.z); w.F32(b.radius);
        w.F32(b.cone_axis.x); w.F32(b.cone_axis.y); w.F32(b.cone_axis.z); w.F32(b.cone_cutoff);
    }
}

// Reads one primitive's cluster hierarchy, checking every count against the bytes left and every reference against what it points into: indices against the
// primitive's vertices, a cluster's triangles against the index array, group references and group members against their tables.
bool ReadClusters(Reader& r, ClusterHierarchy& h, u32 vertex_count, std::string* error) {
    constexpr usize kClusterBytes = 23 * 4;
    constexpr usize kGroupMinBytes = 28;
    u32 index_count = 0, cluster_count = 0, group_count = 0;
    if (!r.U32(h.level_count) || !r.U32(h.leaf_count) || !r.F32(h.mesh_extent) || !r.U32(index_count)) return Fail(error, "truncated cluster header");
    if (h.level_count > 256 || !std::isfinite(h.mesh_extent) || h.mesh_extent < 0.0f) return Fail(error, "bad cluster header");
    if (index_count % 3 != 0 || !r.Fits(index_count, 4)) return Fail(error, "bad cluster indices");
    h.indices.resize(index_count);
    for (u32& i : h.indices) {
        if (!r.U32(i)) return Fail(error, "truncated cluster indices");
        if (i >= vertex_count) return Fail(error, "cluster index out of range");
    }
    if (!r.U32(cluster_count) || !r.Fits(cluster_count, kClusterBytes)) return Fail(error, "truncated clusters");
    if (h.leaf_count > cluster_count) return Fail(error, "cluster leaf count out of range");
    h.clusters.resize(cluster_count);
    const auto finite = [](f32 v) { return std::isfinite(v); };
    for (Cluster& c : h.clusters) {
        r.U32(c.index_offset); r.U32(c.triangle_count); r.U32(c.level);
        r.F32(c.center.x); r.F32(c.center.y); r.F32(c.center.z); r.F32(c.radius); r.F32(c.error);
        r.F32(c.parent_center.x); r.F32(c.parent_center.y); r.F32(c.parent_center.z); r.F32(c.parent_radius); r.F32(c.parent_error);
        r.F32(c.bounds.center.x); r.F32(c.bounds.center.y); r.F32(c.bounds.center.z); r.F32(c.bounds.radius);
        r.F32(c.bounds.cone_axis.x); r.F32(c.bounds.cone_axis.y); r.F32(c.bounds.cone_axis.z); r.F32(c.bounds.cone_cutoff);
        r.I32(c.group); r.I32(c.source_group);
        if (c.index_offset % 3 != 0 || static_cast<u64>(c.index_offset) + 3ull * c.triangle_count > h.indices.size())
            return Fail(error, "cluster triangles out of range");
        if (h.level_count == 0 || c.level >= h.level_count) return Fail(error, "cluster level out of range");
        if (!finite(c.center.x) || !finite(c.center.y) || !finite(c.center.z) || !finite(c.radius) || c.radius < 0.0f || !finite(c.error) || c.error < 0.0f ||
            !finite(c.parent_center.x) || !finite(c.parent_center.y) || !finite(c.parent_center.z) || !finite(c.parent_radius) || c.parent_radius < 0.0f ||
            !finite(c.parent_error) || c.parent_error < 0.0f || !finite(c.bounds.center.x) || !finite(c.bounds.center.y) || !finite(c.bounds.center.z) ||
            !finite(c.bounds.radius) || c.bounds.radius < 0.0f || !finite(c.bounds.cone_axis.x) || !finite(c.bounds.cone_axis.y) ||
            !finite(c.bounds.cone_axis.z) || !finite(c.bounds.cone_cutoff))
            return Fail(error, "non-finite or negative cluster bounds");
    }
    if (!r.U32(group_count) || !r.Fits(group_count, kGroupMinBytes)) return Fail(error, "truncated cluster groups");
    for (const Cluster& c : h.clusters)
        if (c.group < -1 || c.source_group < -1 || c.group >= static_cast<i64>(group_count) || c.source_group >= static_cast<i64>(group_count))
            return Fail(error, "cluster group reference out of range");
    h.groups.resize(group_count);
    for (ClusterGroup& g : h.groups) {
        u32 n = 0;
        for (std::vector<u32>* list : {&g.inputs, &g.outputs}) {
            if (!r.U32(n) || !r.Fits(n, 4)) return Fail(error, "truncated cluster group");
            list->resize(n);
            for (u32& id : *list) {
                if (!r.U32(id)) return Fail(error, "truncated cluster group");
                if (id >= cluster_count) return Fail(error, "cluster group member out of range");
            }
        }
        if (!r.F32(g.center.x) || !r.F32(g.center.y) || !r.F32(g.center.z) || !r.F32(g.radius) || !r.F32(g.error)) return Fail(error, "truncated cluster group");
        if (!finite(g.center.x) || !finite(g.center.y) || !finite(g.center.z) || !finite(g.radius) || g.radius < 0.0f || !finite(g.error) || g.error < 0.0f)
            return Fail(error, "non-finite cluster group bounds");
    }
    return true;
}

bool ReadMeshlets(Reader& r, MeshletMesh& m, u32 vertex_count, std::string* error) {
    u32 n = 0;
    if (!r.U32(n) || !r.Fits(n, 16)) return Fail(error, "truncated meshlet table");
    m.meshlets.resize(n);
    for (Meshlet& ml : m.meshlets) {
        if (!r.U32(ml.vertex_offset) || !r.U32(ml.triangle_offset) || !r.U32(ml.vertex_count) || !r.U32(ml.triangle_count))
            return Fail(error, "truncated meshlet table");
    }
    u32 nv = 0;
    if (!r.U32(nv) || !r.Fits(nv, 4)) return Fail(error, "truncated meshlet vertices");
    m.vertices.resize(nv);
    for (u32& v : m.vertices) {
        if (!r.U32(v)) return Fail(error, "truncated meshlet vertices");
        if (v >= vertex_count) return Fail(error, "meshlet vertex index out of range");
    }
    u32 nt = 0;
    if (!r.U32(nt) || !r.Fits(nt, 1)) return Fail(error, "truncated meshlet triangles");
    m.triangles.resize(nt);
    if (!r.Raw(m.triangles.data(), nt)) return Fail(error, "truncated meshlet triangles");
    if (!r.Fits(n, 32)) return Fail(error, "truncated meshlet bounds");
    m.bounds.resize(n);
    for (MeshletBounds& b : m.bounds) {
        if (!r.F32(b.center.x) || !r.F32(b.center.y) || !r.F32(b.center.z) || !r.F32(b.radius) || !r.F32(b.cone_axis.x) ||
            !r.F32(b.cone_axis.y) || !r.F32(b.cone_axis.z) || !r.F32(b.cone_cutoff))
            return Fail(error, "truncated meshlet bounds");
        if (!std::isfinite(b.center.x) || !std::isfinite(b.center.y) || !std::isfinite(b.center.z) ||
            !std::isfinite(b.radius) || b.radius < 0.0f || !std::isfinite(b.cone_axis.x) ||
            !std::isfinite(b.cone_axis.y) || !std::isfinite(b.cone_axis.z) || !std::isfinite(b.cone_cutoff))
            return Fail(error, "invalid meshlet bounds");
    }
    for (const Meshlet& ml : m.meshlets) {
        const u64 v_end = static_cast<u64>(ml.vertex_offset) + ml.vertex_count;
        const u64 t_end = static_cast<u64>(ml.triangle_offset) + static_cast<u64>(ml.triangle_count) * 3;
        if (v_end > m.vertices.size() || t_end > m.triangles.size()) return Fail(error, "meshlet range out of bounds");
        for (u64 i = ml.triangle_offset; i < t_end; ++i)
            if (m.triangles[i] >= ml.vertex_count) return Fail(error, "meshlet triangle index out of range");
    }
    return true;
}

} // namespace

bool BuildPrimitiveClusters(std::span<const assets::MeshVertex> vertices, std::span<const u32> indices, const ClusterParams& params, ClusterHierarchy& out, std::string* error) {
    out = ClusterHierarchy();
    for (const u32 index : indices) {
        if (index >= vertices.size()) {
            if (error) *error = "an index is past the vertices";
            return false;
        }
    }
    constexpr usize kWeldBytes = offsetof(assets::MeshVertex, tangent); // position, normal and UV
    std::vector<u32> order(vertices.size());
    for (usize i = 0; i < order.size(); ++i) order[i] = static_cast<u32>(i);
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const int c = std::memcmp(&vertices[a], &vertices[b], kWeldBytes);
        return c != 0 ? c < 0 : a < b;
    });
    std::vector<u32> canonical(vertices.size());
    for (usize i = 0; i < order.size();) {
        usize j = i + 1;
        while (j < order.size() && std::memcmp(&vertices[order[i]], &vertices[order[j]], kWeldBytes) == 0) ++j;
        for (usize k = i; k < j; ++k) canonical[order[k]] = order[i];
        i = j;
    }
    std::vector<u32> compact(vertices.size(), 0xFFFFFFFFu);
    std::vector<Vec3> positions;
    std::vector<u32> original;
    for (usize i = 0; i < canonical.size(); ++i) {
        if (canonical[i] != i) continue;
        compact[i] = static_cast<u32>(positions.size());
        positions.push_back(Vec3(vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]));
        original.push_back(static_cast<u32>(i));
    }
    std::vector<u32> welded(indices.size());
    for (usize i = 0; i < welded.size(); ++i) welded[i] = compact[canonical[indices[i]]];
    if (!BuildClusterHierarchy(positions, welded, params, out, error)) return false;
    for (u32& index : out.indices) index = original[index];
    return true;
}

bool CookMesh(const assets::MeshData& mesh, const MeshCookSettings& settings, CookedMesh& out, std::string* error) {
    out = CookedMesh{};
    for (usize p = 0; p < mesh.primitives.size(); ++p) {
        const assets::MeshPrimitiveData& src = mesh.primitives[p];
        const std::string label = "primitive " + std::to_string(p) + ": ";
        CookedMeshPrimitive prim;
        prim.vertices = src.vertices;
        prim.material = src.material;
        prim.bounds_min = src.bounds_min;
        prim.bounds_max = src.bounds_max;

        std::vector<Vec3> positions;
        positions.reserve(src.vertices.size());
        for (const assets::MeshVertex& v : src.vertices) positions.emplace_back(v.position[0], v.position[1], v.position[2]);

        const bool skinned = !src.joints.empty();
        if (skinned) out.warnings.push_back(label + "skinned; LODs and meshlets skipped");

        std::vector<LodLevel> lods;
        std::string reason;
        if (skinned || !settings.build_lods) {
            // Level 0 only; still validates the indices through BuildMeshlets or the check below.
            LodLevel base;
            base.indices = src.indices;
            lods.push_back(std::move(base));
            for (u32 i : src.indices)
                if (i >= src.vertices.size()) return Fail(error, label + "index out of range");
            if (src.indices.size() % 3 != 0) return Fail(error, label + "index count is not a multiple of 3");
        } else if (!GenerateLods(positions, src.indices, settings.lods, lods, &reason)) {
            return Fail(error, label + reason);
        }

        for (LodLevel& level : lods) {
            CookedMeshLod cl;
            cl.indices = std::move(level.indices);
            cl.error = level.error;
            if (settings.build_meshlets && !skinned) {
                if (!BuildMeshlets(positions, cl.indices, settings.meshlets, cl.meshlets, &reason))
                    return Fail(error, label + reason);
            }
            prim.lods.push_back(std::move(cl));
        }
        if (settings.build_clusters && !skinned && !src.indices.empty()) {
            if (!BuildPrimitiveClusters(src.vertices, src.indices, settings.clusters, prim.clusters, &reason)) return Fail(error, label + reason);
        }
        out.primitives.push_back(std::move(prim));
    }
    // Phase 42.4: merge nearby primitives into HLOD proxies (opt-in).
    if (settings.build_hlod && out.primitives.size() >= 2) {
        std::vector<Vec3> centers;
        centers.reserve(out.primitives.size());
        for (const CookedMeshPrimitive& p : out.primitives) centers.push_back((p.bounds_min + p.bounds_max) * 0.5f);
        for (const HlodCluster& cluster : ClusterForHlod(centers, settings.hlod_cluster_radius)) {
            if (cluster.sources.size() < 2) continue; // a lone primitive needs no proxy
            CookedMesh::HlodProxy proxy;
            proxy.center = cluster.center;
            std::vector<u32> offsets;
            offsets.reserve(cluster.sources.size());
            for (usize s : cluster.sources) {
                if (s >= out.primitives.size()) return Fail(error, "hlod source out of range");
                proxy.sources.push_back(static_cast<u32>(s));
                offsets.push_back(static_cast<u32>(proxy.vertices.size()));
                const CookedMeshPrimitive& src = out.primitives[s];
                proxy.vertices.insert(proxy.vertices.end(), src.vertices.begin(), src.vertices.end());
            }
            std::vector<u32> indices;
            for (usize k = 0; k < cluster.sources.size(); ++k) {
                const CookedMeshPrimitive& src = out.primitives[cluster.sources[k]];
                if (src.lods.empty()) continue;
                for (u32 i : src.lods[0].indices) indices.push_back(offsets[k] + i);
            }
            std::vector<Vec3> positions;
            positions.reserve(proxy.vertices.size());
            for (const assets::MeshVertex& v : proxy.vertices)
                positions.emplace_back(v.position[0], v.position[1], v.position[2]);
            std::vector<LodLevel> levels;
            std::string reason;
            if (!GenerateLods(positions, indices, settings.lods, levels, &reason)) return Fail(error, "hlod: " + reason);
            for (LodLevel& level : levels) {
                CookedMeshLod cl;
                cl.indices = std::move(level.indices);
                cl.error = level.error;
                // Meshlets so the virtual-geometry path can draw the proxy too.
                if (settings.build_meshlets && !BuildMeshlets(positions, cl.indices, settings.meshlets, cl.meshlets, &reason))
                    return Fail(error, "hlod: " + reason);
                proxy.lods.push_back(std::move(cl));
            }
            out.hlod.push_back(std::move(proxy));
        }
    }
    return true;
}

std::vector<u8> SaveAmesh(const CookedMesh& mesh) {
    Writer w;
    w.Raw(kMagic, 4);
    w.U32(kVersion);
    w.U32(static_cast<u32>(mesh.primitives.size()));
    for (const CookedMeshPrimitive& p : mesh.primitives) {
        w.I32(p.material);
        w.F32(p.bounds_min.x); w.F32(p.bounds_min.y); w.F32(p.bounds_min.z);
        w.F32(p.bounds_max.x); w.F32(p.bounds_max.y); w.F32(p.bounds_max.z);
        w.U32(static_cast<u32>(p.vertices.size()));
        for (const assets::MeshVertex& v : p.vertices) {
            for (f32 f : v.position) w.F32(f);
            for (f32 f : v.normal) w.F32(f);
            for (f32 f : v.uv) w.F32(f);
            for (f32 f : v.tangent) w.F32(f);
        }
        w.U32(static_cast<u32>(p.lods.size()));
        for (const CookedMeshLod& l : p.lods) {
            w.F32(l.error);
            w.U32(static_cast<u32>(l.indices.size()));
            for (u32 i : l.indices) w.U32(i);
            WriteMeshlets(w, l.meshlets);
        }
    }
    // Phase 42.4: the HLOD proxy table (v2).
    w.U32(static_cast<u32>(mesh.hlod.size()));
    for (const CookedMesh::HlodProxy& p : mesh.hlod) {
        w.F32(p.center.x); w.F32(p.center.y); w.F32(p.center.z);
        w.U32(static_cast<u32>(p.sources.size()));
        for (u32 s : p.sources) w.U32(s);
        w.U32(static_cast<u32>(p.vertices.size()));
        for (const assets::MeshVertex& v : p.vertices) {
            for (f32 f : v.position) w.F32(f);
            for (f32 f : v.normal) w.F32(f);
            for (f32 f : v.uv) w.F32(f);
            for (f32 f : v.tangent) w.F32(f);
        }
        w.U32(static_cast<u32>(p.lods.size()));
        for (const CookedMeshLod& l : p.lods) {
            w.F32(l.error);
            w.U32(static_cast<u32>(l.indices.size()));
            for (u32 i : l.indices) w.U32(i);
            WriteMeshlets(w, l.meshlets);
        }
    }
    // v3: cluster hierarchies, for the primitives that have one.
    u32 with_clusters = 0;
    for (const CookedMeshPrimitive& p : mesh.primitives) with_clusters += p.clusters.clusters.empty() ? 0u : 1u;
    w.U32(with_clusters);
    for (usize i = 0; i < mesh.primitives.size(); ++i) {
        if (!mesh.primitives[i].clusters.clusters.empty()) {
            w.U32(static_cast<u32>(i));
            WriteClusters(w, mesh.primitives[i].clusters);
        }
    }
    const u32 crc = pak::Crc32(w.bytes());
    w.U32(crc);
    return std::move(w.bytes());
}

bool LoadAmesh(std::span<const u8> bytes, CookedMesh& out, std::string* error) {
    out = CookedMesh{};
    if (bytes.size() < 16) return Fail(error, "too small to be a cooked mesh");
    if (std::memcmp(bytes.data(), kMagic, 4) != 0) return Fail(error, "bad magic");
    const std::span<const u8> body = bytes.first(bytes.size() - 4);
    u32 stored = 0;
    for (int i = 0; i < 4; ++i) stored |= static_cast<u32>(bytes[bytes.size() - 4 + i]) << (8 * i);
    if (pak::Crc32(body) != stored) return Fail(error, "checksum mismatch");

    Reader r(body.subspan(4));
    u32 version = 0, prim_count = 0;
    if (!r.U32(version)) return Fail(error, "truncated header");
    if (version < 1 || version > kVersion) return Fail(error, "unsupported version");
    if (!r.U32(prim_count) || !r.Fits(prim_count, 36)) return Fail(error, "truncated primitive table");
    out.primitives.resize(prim_count);
    for (CookedMeshPrimitive& p : out.primitives) {
        u32 vcount = 0, lod_count = 0;
        if (!r.I32(p.material) || !r.F32(p.bounds_min.x) || !r.F32(p.bounds_min.y) || !r.F32(p.bounds_min.z) ||
            !r.F32(p.bounds_max.x) || !r.F32(p.bounds_max.y) || !r.F32(p.bounds_max.z) || !r.U32(vcount))
            return Fail(error, "truncated primitive");
        if (!std::isfinite(p.bounds_min.x) || !std::isfinite(p.bounds_min.y) || !std::isfinite(p.bounds_min.z) ||
            !std::isfinite(p.bounds_max.x) || !std::isfinite(p.bounds_max.y) || !std::isfinite(p.bounds_max.z) ||
            p.bounds_min.x > p.bounds_max.x || p.bounds_min.y > p.bounds_max.y || p.bounds_min.z > p.bounds_max.z)
            return Fail(error, "invalid primitive bounds");
        if (!r.Fits(vcount, sizeof(f32) * 12)) return Fail(error, "truncated vertices");
        p.vertices.resize(vcount);
        for (assets::MeshVertex& v : p.vertices) {
            for (f32& f : v.position) r.F32(f);
            for (f32& f : v.normal) r.F32(f);
            for (f32& f : v.uv) r.F32(f);
            for (f32& f : v.tangent) r.F32(f);
            for (f32 f : v.position) if (!std::isfinite(f)) return Fail(error, "non-finite vertex position");
            for (f32 f : v.normal) if (!std::isfinite(f)) return Fail(error, "non-finite vertex normal");
            for (f32 f : v.uv) if (!std::isfinite(f)) return Fail(error, "non-finite vertex UV");
            for (f32 f : v.tangent) if (!std::isfinite(f)) return Fail(error, "non-finite vertex tangent");
        }
        if (!r.U32(lod_count) || !r.Fits(lod_count, 8)) return Fail(error, "truncated LOD table");
        p.lods.resize(lod_count);
        for (CookedMeshLod& l : p.lods) {
            u32 icount = 0;
            if (!r.F32(l.error) || !std::isfinite(l.error) || l.error < 0.0f || !r.U32(icount) || !r.Fits(icount, 4) ||
                icount % 3 != 0) return Fail(error, "bad LOD indices");
            l.indices.resize(icount);
            for (u32& i : l.indices) {
                if (!r.U32(i)) return Fail(error, "truncated LOD indices");
                if (i >= vcount) return Fail(error, "LOD index out of range");
            }
            if (!ReadMeshlets(r, l.meshlets, vcount, error)) return false;
        }
    }
    // Phase 42.4: the HLOD proxy table (v2; a v1 file simply has none).
    if (version >= 2) {
        u32 hlod_count = 0;
        if (!r.U32(hlod_count) || !r.Fits(hlod_count, 24)) return Fail(error, "truncated HLOD table");
        out.hlod.resize(hlod_count);
        for (CookedMesh::HlodProxy& p : out.hlod) {
            u32 source_count = 0, vcount = 0, lod_count = 0;
            if (!r.F32(p.center.x) || !r.F32(p.center.y) || !r.F32(p.center.z) || !r.U32(source_count) ||
                !r.Fits(source_count, 4)) return Fail(error, "truncated HLOD proxy");
            p.sources.resize(source_count);
            for (u32& s : p.sources) {
                if (!r.U32(s)) return Fail(error, "truncated HLOD sources");
                if (s >= out.primitives.size()) return Fail(error, "HLOD source out of range");
            }
            if (!r.U32(vcount) || !r.Fits(vcount, sizeof(f32) * 12)) return Fail(error, "truncated HLOD vertices");
            p.vertices.resize(vcount);
            for (assets::MeshVertex& v : p.vertices) {
                for (f32& f : v.position) r.F32(f);
                for (f32& f : v.normal) r.F32(f);
                for (f32& f : v.uv) r.F32(f);
                for (f32& f : v.tangent) r.F32(f);
            }
            if (!r.U32(lod_count) || !r.Fits(lod_count, 8)) return Fail(error, "truncated HLOD LOD table");
            p.lods.resize(lod_count);
            for (CookedMeshLod& l : p.lods) {
                u32 icount = 0;
                if (!r.F32(l.error) || !r.U32(icount) || !r.Fits(icount, 4) || icount % 3 != 0)
                    return Fail(error, "bad HLOD LOD indices");
                l.indices.resize(icount);
                for (u32& i : l.indices) {
                    if (!r.U32(i)) return Fail(error, "truncated HLOD LOD indices");
                    if (i >= vcount) return Fail(error, "HLOD LOD index out of range");
                }
                if (!ReadMeshlets(r, l.meshlets, vcount, error)) return false;
            }
        }
    }
    // v3: cluster hierarchies (a v1 or v2 file has none).
    if (version >= 3) {
        u32 count = 0;
        if (!r.U32(count) || count > out.primitives.size() || !r.Fits(count, 4)) return Fail(error, "bad cluster table");
        i64 previous = -1;
        for (u32 n = 0; n < count; ++n) {
            u32 index = 0;
            if (!r.U32(index) || index >= out.primitives.size() || static_cast<i64>(index) <= previous) return Fail(error, "bad cluster primitive index");
            previous = index;
            if (!ReadClusters(r, out.primitives[index].clusters, static_cast<u32>(out.primitives[index].vertices.size()), error)) return false;
        }
    }
    if (r.Remaining() != 0) return Fail(error, "trailing bytes");
    return true;
}

} // namespace aether::cook
