#include "aether/cook/mesh_cook.h"

#include "aether/pak/pak.h"

#include <cstring>

namespace aether::cook {

namespace {

constexpr char kMagic[4] = {'A', 'M', 'S', 'C'};
constexpr u32 kVersion = 1;

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
        out.primitives.push_back(std::move(prim));
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
    if (version != kVersion) return Fail(error, "unsupported version");
    if (!r.U32(prim_count) || !r.Fits(prim_count, 36)) return Fail(error, "truncated primitive table");
    out.primitives.resize(prim_count);
    for (CookedMeshPrimitive& p : out.primitives) {
        u32 vcount = 0, lod_count = 0;
        if (!r.I32(p.material) || !r.F32(p.bounds_min.x) || !r.F32(p.bounds_min.y) || !r.F32(p.bounds_min.z) ||
            !r.F32(p.bounds_max.x) || !r.F32(p.bounds_max.y) || !r.F32(p.bounds_max.z) || !r.U32(vcount))
            return Fail(error, "truncated primitive");
        if (!r.Fits(vcount, sizeof(f32) * 12)) return Fail(error, "truncated vertices");
        p.vertices.resize(vcount);
        for (assets::MeshVertex& v : p.vertices) {
            for (f32& f : v.position) r.F32(f);
            for (f32& f : v.normal) r.F32(f);
            for (f32& f : v.uv) r.F32(f);
            for (f32& f : v.tangent) r.F32(f);
        }
        if (!r.U32(lod_count) || !r.Fits(lod_count, 8)) return Fail(error, "truncated LOD table");
        p.lods.resize(lod_count);
        for (CookedMeshLod& l : p.lods) {
            u32 icount = 0;
            if (!r.F32(l.error) || !r.U32(icount) || !r.Fits(icount, 4) || icount % 3 != 0) return Fail(error, "bad LOD indices");
            l.indices.resize(icount);
            for (u32& i : l.indices) {
                if (!r.U32(i)) return Fail(error, "truncated LOD indices");
                if (i >= vcount) return Fail(error, "LOD index out of range");
            }
            if (!ReadMeshlets(r, l.meshlets, vcount, error)) return false;
        }
    }
    if (r.Remaining() != 0) return Fail(error, "trailing bytes");
    return true;
}

} // namespace aether::cook
