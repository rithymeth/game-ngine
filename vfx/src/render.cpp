#include "aether/vfx/render.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace aether::vfx {

namespace {

Vec3 Normalized(const Vec3& v, const Vec3& fallback) {
    const f32 len = v.Length();
    return len > 1e-8f ? v * (1.0f / len) : fallback;
}

// An axis perpendicular to `n`.
Vec3 Perpendicular(const Vec3& n) {
    const Vec3 helper = std::abs(n.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    return Normalized(helper.Cross(n), {1, 0, 0});
}

// World-space position and direction of particle `i`.
struct Frame {
    const EmitterInstance& e;
    bool local;
    Vec3 Position(usize i) const {
        const Vec3& p = e.Particles().position[i];
        return local ? e.Pose().ToWorld(p) : p;
    }
    Vec3 Velocity(usize i) const {
        const Vec3& v = e.Particles().velocity[i];
        return local ? e.Pose().DirectionToWorld(v) : v;
    }
    Vec3 Direction(const Vec3& d) const { return local ? e.Pose().DirectionToWorld(d) : d; }
};

std::vector<usize> Order(const EmitterInstance& e, const Frame& f, SortMode sort, const ParticleCamera& cam) {
    const ParticleBuffer& p = e.Particles();
    std::vector<usize> order(p.count);
    std::iota(order.begin(), order.end(), usize{0});
    switch (sort) {
    case SortMode::None: break;
    case SortMode::BackToFront:
    case SortMode::FrontToBack: {
        std::vector<f32> depth(p.count);
        for (usize i = 0; i < p.count; ++i) depth[i] = (f.Position(i) - cam.position).Dot(cam.forward);
        if (sort == SortMode::BackToFront) std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) { return depth[a] > depth[b]; });
        else std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) { return depth[a] < depth[b]; });
        break;
    }
    case SortMode::OldestFirst: std::sort(order.begin(), order.end(), [&](usize a, usize b) { return p.id[a] < p.id[b]; }); break;
    case SortMode::NewestFirst: std::sort(order.begin(), order.end(), [&](usize a, usize b) { return p.id[a] > p.id[b]; }); break;
    }
    return order;
}

Mat4 Basis(const Vec3& x, const Vec3& y, const Vec3& z, const Vec3& t) {
    Mat4 m;
    m.cols[0] = Vec4(x.x, x.y, x.z, 0.0f);
    m.cols[1] = Vec4(y.x, y.y, y.z, 0.0f);
    m.cols[2] = Vec4(z.x, z.y, z.z, 0.0f);
    m.cols[3] = Vec4(t.x, t.y, t.z, 1.0f);
    return m;
}

Vec3 RotateAbout(const Vec3& v, const Vec3& axis, f32 angle) {
    // Rodrigues.
    const f32 c = std::cos(angle), s = std::sin(angle);
    return v * c + axis.Cross(v) * s + axis * (axis.Dot(v) * (1.0f - c));
}

void Sprites(const EmitterInstance& e, const Frame& f, const SpriteRenderer& r, const ParticleCamera& cam, ParticleRenderData& out) {
    const ParticleBuffer& p = e.Particles();
    SpriteBatch batch;
    batch.material = r.material;
    batch.blend = r.blend;
    batch.soft_fade = r.soft_fade;
    batch.blend_frames = r.blend_frames;
    batch.instances.reserve(p.count);
    const Vec3 axis = Normalized(f.Direction(r.axis), {0, 1, 0});
    for (usize i : Order(e, f, r.sort, cam)) {
        SpriteInstance s;
        s.center = f.Position(i);
        const f32 h = p.size[i] * 0.5f, w = h * r.aspect;
        Vec3 right = cam.right, up = cam.up;
        switch (r.facing) {
        case SpriteFacing::Camera: break;
        case SpriteFacing::CameraPosition: {
            const Vec3 to_camera = Normalized(cam.position - s.center, cam.forward * -1.0f);
            right = Normalized(cam.up.Cross(to_camera), cam.right);
            up = to_camera.Cross(right);
            break;
        }
        case SpriteFacing::Velocity: {
            const Vec3 vel = f.Velocity(i);
            const f32 speed = vel.Length();
            const Vec3 dir = speed > 1e-6f ? vel * (1.0f / speed) : cam.up;
            const Vec3 to_camera = Normalized(cam.position - s.center, cam.forward * -1.0f);
            right = Normalized(dir.Cross(to_camera), cam.right);
            up = dir;
            // Longer the faster it goes.
            s.right = right * w;
            s.up = up * (h + speed * r.stretch * 0.5f);
            break;
        }
        case SpriteFacing::FixedAxis: {
            const Vec3 to_camera = Normalized(cam.position - s.center, cam.forward * -1.0f);
            up = axis;
            right = Normalized(axis.Cross(to_camera), Perpendicular(axis));
            break;
        }
        case SpriteFacing::FixedPlane:
            right = Perpendicular(axis);
            up = axis.Cross(right);
            break;
        }
        if (r.facing != SpriteFacing::Velocity) {
            // Turned in its own plane by the particle's rotation.
            const f32 c = std::cos(p.rotation[i]), sn = std::sin(p.rotation[i]);
            const Vec3 rr = right * c + up * sn, uu = up * c - right * sn;
            s.right = rr * w;
            s.up = uu * h;
        }
        if (r.camera_offset != 0.0f) s.center = s.center + Normalized(cam.position - s.center, {0, 0, 0}) * r.camera_offset;
        s.color = p.color[i];
        u32 frame = 0, next = 0;
        FlipbookFrame(r, std::min(p.NormalizedAge(i), 1.0f), p.age[i], p.seed[i], frame, next, s.frame_blend);
        s.uv = FlipbookCell(frame, r.columns, r.rows);
        s.uv_next = FlipbookCell(next, r.columns, r.rows);
        if (!r.blend_frames) s.frame_blend = 0.0f;
        batch.instances.push_back(s);
    }
    out.sprites.push_back(std::move(batch));
}

void Meshes(const EmitterInstance& e, const Frame& f, const MeshRenderer& r, const ParticleCamera& cam, ParticleRenderData& out) {
    const ParticleBuffer& p = e.Particles();
    MeshBatch batch;
    batch.mesh = r.mesh;
    batch.material = r.material;
    const Vec3 axis = Normalized(f.Direction(r.axis), {0, 1, 0});
    for (usize i : Order(e, f, r.sort, cam)) {
        const Vec3 pos = f.Position(i);
        Vec3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
        switch (r.orientation) {
        case MeshOrientation::Rotation: {
            const Vec3 base_x = f.Direction({1, 0, 0}), base_y = f.Direction({0, 1, 0}), base_z = f.Direction({0, 0, 1});
            x = RotateAbout(base_x, axis, p.rotation[i]), y = RotateAbout(base_y, axis, p.rotation[i]), z = RotateAbout(base_z, axis, p.rotation[i]);
            break;
        }
        case MeshOrientation::AlignVelocity: {
            y = Normalized(f.Velocity(i), {0, 1, 0});
            x = Perpendicular(y);
            z = x.Cross(y);
            break;
        }
        case MeshOrientation::FaceCamera: {
            z = Normalized(cam.position - pos, cam.forward * -1.0f);
            x = Normalized(cam.up.Cross(z), cam.right);
            y = z.Cross(x);
            break;
        }
        }
        const f32 s = p.size[i];
        batch.transforms.push_back(Basis(x * (s * r.scale.x), y * (s * r.scale.y), z * (s * r.scale.z), pos));
        batch.colors.push_back(p.color[i]);
    }
    out.meshes.push_back(std::move(batch));
}

void Ribbon(const EmitterInstance& e, const Frame& f, const RibbonRenderer& r, const ParticleCamera& cam, ParticleRenderData& out) {
    const ParticleBuffer& p = e.Particles();
    // Oldest to newest.
    std::vector<usize> order(p.count);
    std::iota(order.begin(), order.end(), usize{0});
    std::sort(order.begin(), order.end(), [&](usize a, usize b) { return p.id[a] < p.id[b]; });
    struct Point {
        Vec3 position;
        f32 width;
        LinearColor color;
    };
    std::vector<Point> points;
    points.reserve(order.size() + 1);
    for (usize i : order) points.push_back({f.Position(i), p.size[i] * r.width_scale, p.color[i]});
    if (r.attach_to_emitter && !points.empty()) points.push_back({e.Pose().position, points.back().width, points.back().color});
    if (points.size() < 2) return;
    RibbonStrip strip;
    strip.material = r.material;
    strip.blend = r.blend;
    const Vec3 axis = Normalized(f.Direction(r.axis), {0, 1, 0});
    f32 length = 0.0f, total = 0.0f;
    for (usize k = 1; k < points.size(); ++k) total += (points[k].position - points[k - 1].position).Length();
    Vec3 last_side = Perpendicular(axis);
    for (usize k = 0; k < points.size(); ++k) {
        const Vec3 prev = points[k == 0 ? 0 : k - 1].position, next = points[std::min(k + 1, points.size() - 1)].position;
        const Vec3 tangent = next - prev;
        if (k > 0) length += (points[k].position - points[k - 1].position).Length();
        Vec3 side = r.facing == RibbonFacing::Camera ? tangent.Cross(cam.position - points[k].position) : axis;
        side = Normalized(side, last_side);
        last_side = side;
        const f32 u = r.uv == RibbonUv::Stretch ? (total > 0.0f ? length / total : 0.0f) : length / std::max(r.tile_length, 1e-4f);
        const Vec3 half = side * (points[k].width * 0.5f);
        strip.vertices.push_back({points[k].position - half, points[k].color, u, 0.0f});
        strip.vertices.push_back({points[k].position + half, points[k].color, u, 1.0f});
        if (k > 0) {
            const u32 a = static_cast<u32>(2 * (k - 1)), b = a + 1, c = a + 2, d = a + 3;
            strip.indices.insert(strip.indices.end(), {a, c, b, b, c, d});
        }
    }
    out.ribbons.push_back(std::move(strip));
}

void Lights(const EmitterInstance& e, const Frame& f, const LightRenderer& r, ParticleRenderData& out) {
    const ParticleBuffer& p = e.Particles();
    std::vector<usize> order(p.count);
    std::iota(order.begin(), order.end(), usize{0});
    std::sort(order.begin(), order.end(), [&](usize a, usize b) { return p.id[a] < p.id[b]; });
    u32 made = 0;
    const u64 nth = std::max<u32>(r.every_nth, 1);
    for (usize i : order) {
        if (made >= r.max_lights) break;
        if (p.id[i] % nth != 0) continue;
        LinearColor c = r.use_particle_color ? p.color[i] : r.color;
        out.lights.push_back({f.Position(i), c, p.size[i] * r.radius_scale, r.intensity * c.a});
        ++made;
    }
}

} // namespace

ParticleCamera ParticleCamera::FromView(const Mat4& view) {
    ParticleCamera c;
    c.right = Vec3(view.cols[0].x, view.cols[1].x, view.cols[2].x);
    c.up = Vec3(view.cols[0].y, view.cols[1].y, view.cols[2].y);
    c.forward = Vec3(-view.cols[0].z, -view.cols[1].z, -view.cols[2].z);
    // The view's translation is -R * position.
    const Vec3 t(view.cols[3].x, view.cols[3].y, view.cols[3].z);
    c.position = (c.right * t.x + c.up * t.y - c.forward * t.z) * -1.0f;
    return c;
}

void ParticleRenderData::Clear() {
    sprites.clear();
    meshes.clear();
    ribbons.clear();
    lights.clear();
}

usize ParticleRenderData::DrawnParticles() const {
    usize n = 0;
    for (const auto& b : sprites) n += b.instances.size();
    for (const auto& b : meshes) n += b.transforms.size();
    for (const auto& r : ribbons) n += r.vertices.size() / 2;
    return n;
}

UvRect FlipbookCell(u32 index, u32 columns, u32 rows) {
    columns = std::max(columns, 1u), rows = std::max(rows, 1u);
    index %= columns * rows;
    const f32 w = 1.0f / static_cast<f32>(columns), h = 1.0f / static_cast<f32>(rows);
    return {static_cast<f32>(index % columns) * w, static_cast<f32>(index / columns) * h, w, h};
}

void FlipbookFrame(const SpriteRenderer& r, f32 t, f32 age, u32 seed, u32& frame, u32& next, f32& blend) {
    const u32 cells = std::max(r.columns, 1u) * std::max(r.rows, 1u);
    const u32 count = r.frames == 0 ? cells : std::min(r.frames, cells);
    blend = 0.0f;
    if (count <= 1) {
        frame = next = 0;
        return;
    }
    f32 position = 0.0f;
    switch (r.flipbook) {
    case FlipbookMode::OverLife: position = std::clamp(t, 0.0f, 1.0f) * static_cast<f32>(count) * std::max(r.cycles, 0.0f); break;
    case FlipbookMode::Rate: position = age * std::max(r.fps, 0.0f); break;
    case FlipbookMode::Random:
        frame = next = seed % count;
        return;
    }
    // Once through the frames at the end of life shows the last one, not the first again.
    if (r.flipbook == FlipbookMode::OverLife && r.cycles <= 1.0f) position = std::min(position, static_cast<f32>(count) - 1e-4f);
    const f32 whole = std::floor(position);
    frame = static_cast<u32>(whole) % count;
    next = (frame + 1) % count;
    blend = position - whole;
}

void BuildRenderData(const EmitterInstance& e, const ParticleCamera& cam, ParticleRenderData& out) {
    if (!e.Asset().settings.enabled || e.Count() == 0) return;
    const Frame f{e, e.Asset().settings.space == SimSpace::Local};
    for (const RenderModule& m : e.Asset().render) {
        if (const auto* s = std::get_if<SpriteRenderer>(&m); s != nullptr && s->enabled) Sprites(e, f, *s, cam, out);
        else if (const auto* me = std::get_if<MeshRenderer>(&m); me != nullptr && me->enabled) Meshes(e, f, *me, cam, out);
        else if (const auto* r = std::get_if<RibbonRenderer>(&m); r != nullptr && r->enabled) Ribbon(e, f, *r, cam, out);
        else if (const auto* l = std::get_if<LightRenderer>(&m); l != nullptr && l->enabled) Lights(e, f, *l, out);
    }
}

void BuildRenderData(const ParticleSystemInstance& system, const ParticleCamera& cam, ParticleRenderData& out) {
    for (usize i = 0; i < system.EmitterCount(); ++i) BuildRenderData(system.Emitter(i), cam, out);
}

void ExpandSprites(const SpriteBatch& batch, std::vector<ParticleVertex>& vertices, std::vector<u32>& indices) {
    for (const SpriteInstance& s : batch.instances) {
        const u32 base = static_cast<u32>(vertices.size());
        const UvRect& uv = s.uv;
        // Top-left, top-right, bottom-right, bottom-left.
        vertices.push_back({s.center - s.right + s.up, s.color, uv.x, uv.y});
        vertices.push_back({s.center + s.right + s.up, s.color, uv.x + uv.w, uv.y});
        vertices.push_back({s.center + s.right - s.up, s.color, uv.x + uv.w, uv.y + uv.h});
        vertices.push_back({s.center - s.right - s.up, s.color, uv.x, uv.y + uv.h});
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

} // namespace aether::vfx
