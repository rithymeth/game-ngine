#include "aether/math/math.h"
#include "aether/vfx/render.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>

// Phase 19 step 2: particle draw data - the camera, flipbooks, sprites in
// every facing mode and sort order, meshes, ribbons and lights.

using namespace aether;
using namespace aether::vfx;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }
bool Near(const Vec3& a, const Vec3& b, f32 tol = 1e-3f) { return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol); }
Vec3 Col(const Mat4& m, int c) { return Vec3(m.cols[c].x, m.cols[c].y, m.cols[c].z); }

// Long-lived, size 2, no motion unless given.
Emitter Still() {
    Emitter e;
    e.settings.duration = 1000.0f;
    e.init.push_back(InitLifetime{true, FloatRange::Constant(100.0f)});
    e.init.push_back(InitSize{true, FloatRange::Constant(2.0f)});
    return e;
}

// Particles spread along X from -5 to 5 (random order of birth).
Emitter Line() {
    Emitter e = Still();
    e.init.push_back(InitShape{true, ShapeKind::Edge, 5.0f, {}, 1.0f});
    return e;
}

ParticleCamera Camera(const Vec3& eye, const Vec3& target = {0, 0, 0}) { return ParticleCamera::FromView(Mat4::LookAtRH(eye, target, {0, 1, 0})); }

const SpriteBatch& OnlySprites(const ParticleRenderData& d) { return d.sprites.at(0); }

} // namespace

AETHER_TEST(VFX_RenderSpritesAndFlipbooks) {
    // The camera from a view matrix.
    const ParticleCamera front = Camera({0, 0, 10});
    CHECK(Near(front.position, {0, 0, 10}) && Near(front.forward, {0, 0, -1}) && Near(front.right, {1, 0, 0}) && Near(front.up, {0, 1, 0}));
    const ParticleCamera side = Camera({20, 0, 0});
    CHECK(Near(side.position, {20, 0, 0}) && Near(side.forward, {-1, 0, 0}) && Near(side.right, {0, 0, -1}) && Near(side.up, {0, 1, 0}));
    const ParticleCamera moved = Camera({3, 4, 5}, {3, 4, 0});
    CHECK(Near(moved.position, {3, 4, 5}) && Near(moved.forward, {0, 0, -1}));

    // Flipbooks: cells left to right then down; frames over life, by rate, or random.
    const UvRect cell = FlipbookCell(5, 4, 2);
    CHECK(Near(cell.x, 0.25f) && Near(cell.y, 0.5f) && Near(cell.w, 0.25f) && Near(cell.h, 0.5f) && Near(FlipbookCell(9, 4, 2).x, 0.25f));
    SpriteRenderer book;
    book.columns = 4, book.rows = 2;
    u32 frame = 0, next = 0;
    f32 blend = 0.0f;
    FlipbookFrame(book, 0.5f, 0.0f, 0, frame, next, blend);
    CHECK(frame == 4 && next == 5 && Near(blend, 0.0f));
    FlipbookFrame(book, 0.55f, 0.0f, 0, frame, next, blend);
    CHECK(frame == 4 && Near(blend, 0.4f));
    FlipbookFrame(book, 1.0f, 0.0f, 0, frame, next, blend);
    CHECK(frame == 7); // the last frame at the end, not the first again
    book.cycles = 2.0f;
    FlipbookFrame(book, 0.75f, 0.0f, 0, frame, next, blend);
    CHECK(frame == 4);
    book.flipbook = FlipbookMode::Rate;
    book.fps = 10.0f;
    FlipbookFrame(book, 0.0f, 0.35f, 0, frame, next, blend);
    CHECK(frame == 3 && next == 4 && Near(blend, 0.5f));
    FlipbookFrame(book, 0.0f, 0.75f, 0, frame, next, blend);
    CHECK(frame == 7 && next == 0); // wraps
    book.flipbook = FlipbookMode::Random;
    FlipbookFrame(book, 0.3f, 0.1f, 13, frame, next, blend);
    CHECK(frame == 5 && next == 5 && blend == 0.0f);
    book.flipbook = FlipbookMode::OverLife;
    book.cycles = 1.0f;
    book.frames = 6;
    FlipbookFrame(book, 1.0f, 0.0f, 0, frame, next, blend);
    CHECK(frame == 5);
    SpriteRenderer single;
    FlipbookFrame(single, 0.7f, 3.0f, 9, frame, next, blend);
    CHECK(frame == 0 && next == 0 && blend == 0.0f);

    // Camera-facing sprites: the camera's axes, half the size each way.
    Emitter e = Line();
    e.init.push_back(InitColor{true, ColorGradient::Constant({1, 0.5f, 0.25f, 0.8f})});
    SpriteRenderer sprites;
    sprites.material = "M_Smoke";
    sprites.blend = BlendMode::Additive;
    sprites.soft_fade = 0.5f;
    e.render = {sprites};
    EmitterInstance line(e, 3);
    line.Emit(20);
    ParticleRenderData data;
    BuildRenderData(line, side, data);
    CHECK(data.sprites.size() == 1 && OnlySprites(data).instances.size() == 20 && OnlySprites(data).material == "M_Smoke" &&
          OnlySprites(data).blend == BlendMode::Additive && OnlySprites(data).soft_fade == 0.5f && data.DrawnParticles() == 20);
    const SpriteInstance& s0 = OnlySprites(data).instances[0];
    CHECK(Near(s0.right, {0, 0, -1}) && Near(s0.up, {0, 1, 0}) && Near(s0.color.g, 0.5f) && Near(s0.color.a, 0.8f) && s0.uv.w == 1.0f);
    // Back to front along the view: from -X (far) to +X (near).
    bool back_to_front = true;
    for (usize i = 1; i < 20; ++i) back_to_front &= OnlySprites(data).instances[i].center.x >= OnlySprites(data).instances[i - 1].center.x;
    CHECK(back_to_front);
    auto sorted = [&](SortMode mode) {
        Emitter copy = e;
        std::get<SpriteRenderer>(copy.render[0]).sort = mode;
        EmitterInstance inst(copy, 3);
        inst.Emit(20);
        ParticleRenderData d;
        BuildRenderData(inst, side, d);
        std::vector<Vec3> centers;
        for (const SpriteInstance& s : d.sprites[0].instances) centers.push_back(s.center);
        return centers;
    };
    const std::vector<Vec3> front_first = sorted(SortMode::FrontToBack);
    CHECK(std::is_sorted(front_first.begin(), front_first.end(), [](const Vec3& a, const Vec3& b) { return a.x > b.x; }));
    // Oldest first is birth order; with no deaths, that's the buffer's order.
    const std::vector<Vec3> oldest = sorted(SortMode::OldestFirst), newest = sorted(SortMode::NewestFirst);
    bool reversed = true;
    for (usize i = 0; i < 20; ++i) reversed &= Near(oldest[i], newest[19 - i]);
    CHECK(reversed && Near(oldest[0], line.Particles().position[0]));

    // Rotation turns it in its plane; aspect widens it.
    Emitter turned = Still();
    turned.init.push_back(InitRotation{true, FloatRange::Constant(Radians(90.0f)), {}});
    SpriteRenderer wide;
    wide.aspect = 2.0f;
    wide.sort = SortMode::None;
    turned.render = {wide};
    EmitterInstance spin(turned);
    spin.Emit(1);
    data.Clear();
    BuildRenderData(spin, front, data);
    CHECK(Near(OnlySprites(data).instances[0].right, {0, 2, 0}) && Near(OnlySprites(data).instances[0].up, {-1, 0, 0}));

    // Facing modes.
    auto one = [&](const SpriteRenderer& r, const ParticleCamera& cam, InitModule extra = InitLifetime{true, FloatRange::Constant(100.0f)},
                   EmitterPose pose = {}) {
        Emitter em = Still();
        em.init.push_back(extra);
        em.render = {r};
        EmitterInstance inst(em);
        inst.Teleport(pose);
        inst.Emit(1);
        ParticleRenderData d;
        BuildRenderData(inst, cam, d);
        return d.sprites[0].instances[0];
    };
    SpriteRenderer velocity;
    velocity.facing = SpriteFacing::Velocity;
    velocity.stretch = 0.1f;
    const SpriteInstance fast = one(velocity, side, InitVelocity{true, VelocityMode::Direction, {0, 1, 0}, 0.0f, FloatRange::Constant(5.0f)});
    CHECK(Near(fast.up, {0, 1.25f, 0}) && Near(fast.right.Length(), 1.0f) && Near(fast.right.Dot({1, 0, 0}), 0.0f));
    SpriteRenderer axis;
    axis.facing = SpriteFacing::FixedAxis;
    const SpriteInstance beam = one(axis, side);
    CHECK(Near(beam.up, {0, 1, 0}) && Near(beam.right, {0, 0, -1}));
    SpriteRenderer plane;
    plane.facing = SpriteFacing::FixedPlane;
    const SpriteInstance ripple = one(plane, side);
    CHECK(Near(ripple.right.y, 0.0f) && Near(ripple.up.y, 0.0f) && Near(ripple.right.Dot(ripple.up), 0.0f) && Near(ripple.right.Length(), 1.0f));
    SpriteRenderer toward;
    toward.facing = SpriteFacing::CameraPosition;
    const SpriteInstance faced = one(toward, Camera({0, 10, 10}, {0, 10, 0}), InitLifetime{true, FloatRange::Constant(100.0f)});
    const Vec3 to_camera = Vec3(0, 10, 10).Normalized();
    CHECK(Near(faced.right.Dot(to_camera), 0.0f) && Near(faced.up.Dot(to_camera), 0.0f) && Near(faced.right.Length(), 1.0f));
    // Local simulation: drawn where the emitter is; pulled towards the camera by camera_offset.
    SpriteRenderer offset;
    offset.camera_offset = 1.0f;
    Emitter local = Still();
    local.settings.space = SimSpace::Local;
    local.render = {offset};
    EmitterInstance carried(local);
    carried.Teleport({{0, 5, 0}, {}});
    carried.Emit(1);
    data.Clear();
    BuildRenderData(carried, Camera({0, 5, 10}, {0, 5, 0}), data);
    CHECK(Near(OnlySprites(data).instances[0].center, {0, 5, 1}));

    // Flipbook frames in the instances, blended when asked.
    Emitter animated = Still();
    SpriteRenderer frames;
    frames.columns = 4, frames.rows = 2;
    frames.blend_frames = true;
    animated.render = {frames};
    EmitterInstance anim(animated);
    anim.Emit(1);
    anim.Update(55.0f); // 55% of its 100 s life: frame 4, 40% of the way to 5
    data.Clear();
    BuildRenderData(anim, front, data);
    const SpriteInstance& f = OnlySprites(data).instances[0];
    CHECK(Near(f.uv.x, 0.0f) && Near(f.uv.y, 0.5f) && Near(f.uv_next.x, 0.25f) && Near(f.frame_blend, 0.4f) && OnlySprites(data).blend_frames);

    // As plain triangles.
    std::vector<ParticleVertex> vertices;
    std::vector<u32> indices;
    ExpandSprites(OnlySprites(data), vertices, indices);
    CHECK(vertices.size() == 4 && indices.size() == 6 && Near(vertices[0].position, {-1, 1, 0}) && Near(vertices[2].position, {1, -1, 0}));
    CHECK(Near(vertices[1].u, 0.25f) && Near(vertices[3].v, 1.0f) && indices[4] == 2 && indices[5] == 3);

    // Nothing drawn for disabled renderers, disabled emitters or no particles.
    Emitter off = Line();
    SpriteRenderer disabled;
    disabled.enabled = false;
    off.render = {disabled};
    EmitterInstance quiet(off);
    quiet.Emit(5);
    data.Clear();
    BuildRenderData(quiet, front, data);
    EmitterInstance empty(e);
    BuildRenderData(empty, front, data);
    CHECK(data.sprites.empty() && data.DrawnParticles() == 0);
}

AETHER_TEST(VFX_RenderMeshesRibbonsAndLights) {
    const ParticleCamera side = Camera({20, 0, 0});
    // Meshes: turned by the particle's rotation, scaled by its size.
    Emitter rocks = Still();
    rocks.init.push_back(InitRotation{true, FloatRange::Constant(Radians(90.0f)), {}});
    rocks.render = {MeshRenderer{true, "SM_Rock", "M_Rock", MeshOrientation::Rotation, {0, 1, 0}, {1, 2, 3}}};
    EmitterInstance rock(rocks);
    rock.Teleport({{1, 2, 3}, {}});
    rock.Emit(1);
    ParticleRenderData data;
    BuildRenderData(rock, side, data);
    CHECK(data.meshes.size() == 1 && data.meshes[0].mesh == "SM_Rock" && data.meshes[0].material == "M_Rock" && data.meshes[0].transforms.size() == 1);
    const Mat4& m = data.meshes[0].transforms[0];
    CHECK(Near(Col(m, 0), {0, 0, -2}) && Near(Col(m, 1), {0, 4, 0}) && Near(Col(m, 2), {6, 0, 0}) && Near(Col(m, 3), {1, 2, 3}) && data.DrawnParticles() == 1);
    // Along their velocity, or facing the camera.
    Emitter darts = Still();
    darts.init.push_back(InitVelocity{true, VelocityMode::Direction, {1, 0, 0}, 0.0f, FloatRange::Constant(3)});
    darts.render = {MeshRenderer{true, "SM_Dart", "", MeshOrientation::AlignVelocity}, MeshRenderer{true, "SM_Card", "", MeshOrientation::FaceCamera}};
    EmitterInstance dart(darts);
    dart.Emit(1);
    data.Clear();
    BuildRenderData(dart, side, data);
    CHECK(data.meshes.size() == 2 && Near(Col(data.meshes[0].transforms[0], 1), {2, 0, 0}) && Near(Col(data.meshes[1].transforms[0], 2), {2, 0, 0}));
    CHECK(Near(Col(data.meshes[1].transforms[0], 0).Dot({1, 0, 0}), 0.0f));

    // Ribbons: a strip through the particles in birth order.
    Emitter trail = Still();
    trail.init.back() = InitSize{true, FloatRange::Constant(0.5f)};
    trail.spawn.push_back(SpawnPerDistance{true, 2.0f});
    RibbonRenderer ribbon;
    ribbon.material = "M_Trail";
    ribbon.width_scale = 2.0f;
    trail.render = {ribbon};
    EmitterInstance trails(trail);
    trails.Teleport({{0, 0, 0}, {}});
    trails.SetPose({{10, 0, 0}, {}});
    trails.Update(1.0f);
    CHECK(trails.Count() == 20);
    data.Clear();
    const ParticleCamera above = Camera({5, 0, 20}, {5, 0, 0});
    BuildRenderData(trails, above, data);
    CHECK(data.ribbons.size() == 1 && data.ribbons[0].material == "M_Trail" && data.ribbons[0].vertices.size() == 40 && data.ribbons[0].indices.size() == 6 * 19);
    const auto& v = data.ribbons[0].vertices;
    // Points run along +X; the strip spreads across the view (Y here), one unit wide (size 0.5 x 2).
    bool along = true, across = true;
    for (usize k = 0; k < 20; ++k) {
        if (k > 0) along &= v[2 * k].position.x > v[2 * (k - 1)].position.x;
        across &= Near(std::fabs(v[2 * k].position.y - v[2 * k + 1].position.y), 1.0f) && Near(v[2 * k].position.z, 0.0f);
    }
    CHECK(along && across && Near(v[0].u, 0.0f) && Near(v.back().u, 1.0f) && v[0].v == 0.0f && v[1].v == 1.0f);
    // Tiled by distance, fixed to an axis, and joined to the emitter.
    std::get<RibbonRenderer>(trail.render[0]).uv = RibbonUv::Distance;
    std::get<RibbonRenderer>(trail.render[0]).tile_length = 2.0f;
    std::get<RibbonRenderer>(trail.render[0]).facing = RibbonFacing::Axis;
    std::get<RibbonRenderer>(trail.render[0]).axis = {0, 0, 1};
    std::get<RibbonRenderer>(trail.render[0]).attach_to_emitter = true;
    EmitterInstance tiled(trail);
    tiled.Teleport({{0, 0, 0}, {}});
    tiled.SetPose({{10, 0, 0}, {}});
    tiled.Update(1.0f);
    data.Clear();
    BuildRenderData(tiled, above, data);
    const auto& tv = data.ribbons[0].vertices;
    CHECK(tv.size() == 42 && Near(tv.back().position.x, 10.0f) && Near(tv.back().u, (10.0f - tv[0].position.x) / 2.0f) && Near(std::fabs(tv[0].position.z), 0.5f));
    // One point makes no strip.
    Emitter single = trail;
    std::get<RibbonRenderer>(single.render[0]).attach_to_emitter = false;
    EmitterInstance lonely(single);
    lonely.Emit(1);
    data.Clear();
    BuildRenderData(lonely, above, data);
    CHECK(data.ribbons.empty());

    // Lights: every nth particle, up to a cap, sized by the particle.
    Emitter sparks = Still();
    sparks.init.push_back(InitColor{true, ColorGradient::Constant({1, 0.6f, 0.2f, 0.5f})});
    LightRenderer light;
    light.every_nth = 2;
    light.max_lights = 3;
    light.radius_scale = 4.0f;
    light.intensity = 2.0f;
    LightRenderer fixed;
    fixed.use_particle_color = false;
    fixed.color = {0, 0, 1, 1};
    fixed.max_lights = 1;
    sparks.render = {light, fixed};
    EmitterInstance spark(sparks);
    spark.Emit(10);
    data.Clear();
    BuildRenderData(spark, side, data);
    CHECK(data.lights.size() == 4 && Near(data.lights[0].radius, 8.0f) && Near(data.lights[0].intensity, 1.0f) && Near(data.lights[0].color.g, 0.6f));
    CHECK(data.lights[3].color.b == 1.0f && data.lights[3].color.r == 0.0f && Near(data.lights[3].intensity, 1.0f) && data.DrawnParticles() == 0);

    // A whole system appends each emitter's draw data.
    ParticleSystemAsset asset;
    asset.emitters = {rocks, sparks};
    ParticleSystemInstance system(asset);
    system.Emitter(0).Emit(2);
    system.Emitter(1).Emit(4);
    data.Clear();
    BuildRenderData(system, side, data);
    CHECK(data.meshes.size() == 1 && data.meshes[0].transforms.size() == 2 && data.lights.size() == 3);
}
