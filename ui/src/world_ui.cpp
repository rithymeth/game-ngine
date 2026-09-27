#include "aether/ui/world_ui.h"

#include "aether/ecs/component.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aether::ui {

namespace {
WorldUISystem* g_world_active = nullptr;

Vec4 Mul(const Mat4& m, const Vec3& p, f32 w = 1.0f) { return m * Vec4(p.x, p.y, p.z, w); }
Vec3 Col(const Mat4& m, int c) { return Vec3(m.cols[c].x, m.cols[c].y, m.cols[c].z); }

// A general 4x4 inverse (cofactors), for unprojecting pointer rays.
Mat4 Inverse(const Mat4& in) {
    f32 m[16], inv[16];
    for (int c = 0; c < 4; ++c) {
        m[c * 4 + 0] = in.cols[c].x;
        m[c * 4 + 1] = in.cols[c].y;
        m[c * 4 + 2] = in.cols[c].z;
        m[c * 4 + 3] = in.cols[c].w;
    }
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const f32 det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 out;
    if (std::abs(det) < 1e-20f) return out;
    const f32 k = 1.0f / det;
    for (int c = 0; c < 4; ++c) out.cols[c] = Vec4(inv[c * 4 + 0] * k, inv[c * 4 + 1] * k, inv[c * 4 + 2] * k, inv[c * 4 + 3] * k);
    return out;
}

Vec3 Unproject(const Mat4& inverse_vp, f32 ndc_x, f32 ndc_y, f32 depth) {
    const Vec4 p = inverse_vp * Vec4(ndc_x, ndc_y, depth, 1.0f);
    const f32 w = std::abs(p.w) > 1e-20f ? p.w : 1e-20f;
    return Vec3(p.x / w, p.y / w, p.z / w);
}
} // namespace

void RegisterWorldWidgetComponents() { (void)GetComponentId<WorldWidgetComponent>(); }

WorldUISystem* WorldUISystem::Active() { return g_world_active; }
void WorldUISystem::MakeActive() { g_world_active = this; }

WorldUISystem::WorldUISystem(World& world, const Font& font, UISystem::LayoutLoader loader, const Theme* theme, const FontLibrary* fonts, const GuidIndex* guids)
    : world_(world), font_(font), loader_(std::move(loader)), theme_(theme), fonts_(fonts), guids_(guids) {
    RegisterWorldWidgetComponents();
    MakeActive();
}

WorldUISystem::~WorldUISystem() {
    instances_.clear();
    if (g_world_active == this) g_world_active = nullptr;
}

void WorldUISystem::Report(const std::string& key, const std::string& message) {
    if (reported_[key]) return;
    reported_[key] = true;
    problems_.push_back(message);
}

WorldUISystem::Instance* WorldUISystem::Find(Entity e) {
    const auto it = instances_.find(e.index);
    return it == instances_.end() || it->second->entity != e ? nullptr : it->second.get();
}
const WorldUISystem::Instance* WorldUISystem::Find(Entity e) const { return const_cast<WorldUISystem*>(this)->Find(e); }

const WorldUISystem::Placement* WorldUISystem::PlacementOf(Entity e) const {
    const Instance* in = Find(e);
    return in != nullptr ? &in->placement : nullptr;
}

usize WorldUISystem::ShownCount() const {
    usize n = 0;
    for (const auto& [i, in] : instances_) n += in->placement.shown ? 1 : 0;
    return n;
}

Widget* WorldUISystem::RootOf(Entity e) const {
    const Instance* in = Find(e);
    return in != nullptr ? in->root : nullptr;
}

Widget* WorldUISystem::FindWidget(Entity e, const std::string& widget) const {
    Widget* root = RootOf(e);
    return root != nullptr ? root->Find(widget) : nullptr;
}

UIAnimator* WorldUISystem::AnimatorOf(Entity e) const {
    const Instance* in = Find(e);
    return in != nullptr ? in->animator.get() : nullptr;
}

DataBinder* WorldUISystem::BinderOf(Entity e) const {
    const Instance* in = Find(e);
    return in != nullptr ? in->binder.get() : nullptr;
}

UIInputRouter* WorldUISystem::RouterOf(Entity e) const {
    const Instance* in = Find(e);
    return in != nullptr ? in->router.get() : nullptr;
}

void WorldUISystem::AddGlobalSource(const std::string& name, void* object, const reflect::TypeInfo& type) { globals_[name] = {object, &type}; }

bool WorldUISystem::Create(Instance& in) {
    LayoutDocument doc;
    std::string error;
    if (!loader_ || !loader_(in.layout, doc, &error) || !doc.root) {
        Report("layout:" + in.layout, "layout '" + in.layout + "': " + (error.empty() ? std::string("couldn't be loaded") : error));
        return false;
    }
    if (theme_ != nullptr) ApplyTheme(*theme_, *doc.root);
    // Its own viewport, one layout unit to the pixel: the system scales it when placing it.
    in.viewport.scale_settings.rule = ScaleSettings::Rule::None;
    in.viewport.scale_settings.min_scale = 1.0f;
    in.viewport.SetSize({in.settings.width, in.settings.height});
    in.viewport.SetFonts(fonts_);
    in.root = in.viewport.Add(std::move(doc.root));
    in.router = std::make_unique<UIInputRouter>(in.viewport, font_);
    in.animator = std::make_unique<UIAnimator>(*in.root);
    for (UIAnimation& a : doc.animations) {
        for (const std::string& p : in.animator->Validate(a)) problems_.push_back("layout '" + in.layout + "': " + p);
        in.animator->Add(std::move(a));
    }
    in.binder = std::make_unique<DataBinder>();
    in.bindings = doc.bindings;
    in.sources = detail::BindingSourceNames(in.bindings);
    detail::RefreshBindingSources(world_, in.entity, in.sources, globals_, *in.binder);
    for (const std::string& p : in.binder->Bind(*in.root, in.bindings)) problems_.push_back("layout '" + in.layout + "': " + p);
    detail::HookWidgetEvents(*in.root, in.entity, pending_);
    return true;
}

void WorldUISystem::Place(Instance& in, const WorldCamera& cam) {
    Placement& p = in.placement;
    p = {};
    const WorldWidgetComponent& s = in.settings;
    if (!s.visible || in.root == nullptr || s.width <= 0.0f || s.height <= 0.0f) return;

    // The entity's place (and facing, for World widgets that don't turn to the camera).
    Mat4 world_transform;
    if (guids_ != nullptr) {
        world_transform = ComputeWorldTransform(world_, *guids_, in.entity);
    } else if (const Transform* t = world_.GetComponent<Transform>(in.entity)) {
        world_transform = Mat4::Translation(t->position) * t->rotation.ToMat4();
    } else {
        Report("transform:" + std::to_string(in.entity.index), "world widget '" + in.layout + "': its entity has no Transform");
        return;
    }
    const Vec3 anchor = Col(world_transform, 3) + s.offset;
    const Mat4 vp = cam.projection * cam.view;
    p.distance = (anchor - camera_position_).Length();
    if (s.max_distance > 0.0f && p.distance > s.max_distance) return;
    const Vec4 clip = Mul(vp, anchor);

    if (s.space == WorldWidgetSpace::Screen) {
        if (clip.w <= 1e-5f) return; // behind the camera
        const f32 nx = clip.x / clip.w, ny = clip.y / clip.w, nz = clip.z / clip.w;
        if (nz < 0.0f || nz > 1.0f) return; // nearer than the near plane or past the far one
        p.depth = nz;
        p.anchor = {(nx * 0.5f + 0.5f) * cam.screen.x, (0.5f - ny * 0.5f) * cam.screen.y};
        f32 k = 1.0f;
        if (s.reference_distance > 0.0f && p.distance > s.reference_distance) k = std::max(s.reference_distance / p.distance, s.min_scale);
        p.scale = cam.ui_scale * k;
        // Whole pixels, so text stays crisp as the entity moves.
        p.rect = {std::round(p.anchor.x - s.pivot_x * s.width * p.scale), std::round(p.anchor.y - s.pivot_y * s.height * p.scale), s.width * p.scale,
                  s.height * p.scale};
        if (p.rect.Right() < 0.0f || p.rect.Bottom() < 0.0f || p.rect.x > cam.screen.x || p.rect.y > cam.screen.y) return;
        p.shown = true;
        return;
    }

    // World: a quad through the anchor, facing the camera or turned with the entity.
    Vec3 right, up;
    if (s.billboard) {
        right = Vec3(cam.view.cols[0].x, cam.view.cols[1].x, cam.view.cols[2].x);
        up = Vec3(cam.view.cols[0].y, cam.view.cols[1].y, cam.view.cols[2].y);
    } else {
        right = Col(world_transform, 0).Normalized();
        up = Col(world_transform, 1).Normalized();
    }
    const f32 wm = s.world_width, hm = s.world_width * s.height / s.width;
    const Vec3 origin = anchor - right * (s.pivot_x * wm) + up * (s.pivot_y * hm);
    const Vec3 ax = right * (wm / s.width), ay = up * (-hm / s.height);
    const Vec3 normal = right.Cross(up);
    p.transform.cols[0] = Vec4(ax.x, ax.y, ax.z, 0.0f);
    p.transform.cols[1] = Vec4(ay.x, ay.y, ay.z, 0.0f);
    p.transform.cols[2] = Vec4(normal.x, normal.y, normal.z, 0.0f);
    p.transform.cols[3] = Vec4(origin.x, origin.y, origin.z, 1.0f);
    p.corners[0] = origin;
    p.corners[1] = origin + right * wm;
    p.corners[2] = origin + right * wm - up * hm;
    p.corners[3] = origin - up * hm;
    bool in_front = false;
    for (const Vec3& c : p.corners) in_front |= Mul(vp, c).w > 1e-5f;
    if (!in_front) return;
    p.depth = clip.w > 1e-5f ? std::clamp(clip.z / clip.w, 0.0f, 1.0f) : 0.0f;
    p.shown = true;
}

void WorldUISystem::Update(const WorldCamera& cam, f32 dt) {
    const u64 now = ++generation_;
    events_ = std::move(pending_);
    pending_.clear();
    const Mat4 inverse_view = Inverse(cam.view);
    camera_position_ = Col(inverse_view, 3);
    inverse_view_projection_ = Inverse(cam.projection * cam.view);
    screen_ = cam.screen;

    std::vector<Entity> entities;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<WorldWidgetComponent>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    for (Entity e : entities) {
        const WorldWidgetComponent& wc = *world_.GetComponent<WorldWidgetComponent>(e);
        auto& slot = instances_[e.index];
        if (slot && (slot->entity != e || slot->layout != wc.layout)) {
            if (captured_ == slot->entity) captured_ = kNullEntity;
            if (hovered_ == slot->entity) hovered_ = kNullEntity;
            slot.reset(); // a recycled entity, or a new layout
        }
        if (!slot) {
            slot = std::make_unique<Instance>();
            slot->entity = e;
            slot->layout = wc.layout;
            slot->settings = wc;
            Create(*slot);
        }
        Instance& in = *slot;
        in.seen = now;
        in.settings = wc;
        if (in.root == nullptr) continue;
        in.viewport.SetSize({wc.width, wc.height});
        detail::RefreshBindingSources(world_, e, in.sources, globals_, *in.binder);
        in.binder->Update();
        in.animator->Tick(dt);
        for (const std::string& name : in.animator->Finished()) events_.push_back({e, WidgetEvent::Kind::AnimationFinished, name, 0.0f, {}});
        in.router->Tick(dt);
        in.viewport.Layout(font_);
        Place(in, cam);
    }
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (it->second->seen == now) {
            ++it;
            continue;
        }
        if (captured_ == it->second->entity) captured_ = kNullEntity;
        if (hovered_ == it->second->entity) hovered_ = kNullEntity;
        it = instances_.erase(it);
    }
}

DrawList WorldUISystem::PaintScreen() const {
    std::vector<const Instance*> shown;
    for (const auto& [i, in] : instances_) {
        if (in->placement.shown && in->settings.space == WorldWidgetSpace::Screen) shown.push_back(in.get());
    }
    std::stable_sort(shown.begin(), shown.end(), [](const Instance* a, const Instance* b) { return a->placement.distance > b->placement.distance; });
    DrawList list;
    const PaintContext ctx{&font_, 1.0f, fonts_};
    for (const Instance* in : shown) {
        const Placement& p = in->placement;
        list.PushTransform({p.rect.x, p.rect.y}, {p.scale, p.scale}, {0.0f, 0.0f});
        list.PushClip({0.0f, 0.0f, in->settings.width, in->settings.height});
        for (usize l = 0; l < in->viewport.LayerCount(); ++l) in->viewport.Layer(l)->Paint(list, ctx);
        list.PopClip();
        list.PopTransform();
    }
    return list;
}

std::vector<WorldUISystem::WorldDraw> WorldUISystem::PaintWorld() const {
    std::vector<WorldDraw> out;
    const PaintContext ctx{&font_, 1.0f, fonts_};
    for (const auto& [i, in] : instances_) {
        if (!in->placement.shown || in->settings.space != WorldWidgetSpace::World) continue;
        WorldDraw d;
        d.entity = in->entity;
        d.transform = in->placement.transform;
        d.size = {in->settings.width, in->settings.height};
        d.distance = in->placement.distance;
        d.list.PushClip({0.0f, 0.0f, d.size.x, d.size.y});
        for (usize l = 0; l < in->viewport.LayerCount(); ++l) in->viewport.Layer(l)->Paint(d.list, ctx);
        d.list.PopClip();
        out.push_back(std::move(d));
    }
    std::stable_sort(out.begin(), out.end(), [](const WorldDraw& a, const WorldDraw& b) { return a.distance > b.distance; });
    return out;
}

bool WorldUISystem::ToLocal(const Instance& in, Vec2 pixel, Vec2& local, f32& distance) const {
    const Placement& p = in.placement;
    if (in.settings.space == WorldWidgetSpace::Screen) {
        local = {(pixel.x - p.rect.x) / p.scale, (pixel.y - p.rect.y) / p.scale};
        distance = p.distance;
        return true;
    }
    // A ray from the pixel against the quad's plane.
    const f32 nx = pixel.x / std::max(screen_.x, 1.0f) * 2.0f - 1.0f, ny = 1.0f - pixel.y / std::max(screen_.y, 1.0f) * 2.0f;
    const Vec3 a = Unproject(inverse_view_projection_, nx, ny, 0.0f), b = Unproject(inverse_view_projection_, nx, ny, 1.0f);
    const Vec3 dir = b - a;
    const Vec3 origin = Col(p.transform, 3), ax = Col(p.transform, 0), ay = Col(p.transform, 1);
    const Vec3 normal = ax.Cross(ay);
    const f32 denom = dir.Dot(normal);
    if (std::abs(denom) < 1e-12f) return false; // edge on
    const f32 t = (origin - a).Dot(normal) / denom;
    if (t < 0.0f) return false; // behind the camera
    const Vec3 hit = a + dir * t;
    local = {(hit - origin).Dot(ax) / ax.LengthSq(), (hit - origin).Dot(ay) / ay.LengthSq()};
    distance = (hit - camera_position_).Length();
    return true;
}

WorldUISystem::Hit WorldUISystem::HitTest(Vec2 pixel) const {
    Hit best;
    best.distance = std::numeric_limits<f32>::max();
    for (const auto& [i, in] : instances_) {
        if (!in->placement.shown) continue;
        Vec2 local;
        f32 distance = 0.0f;
        if (!ToLocal(*in, pixel, local, distance) || distance >= best.distance) continue;
        if (local.x < 0.0f || local.y < 0.0f || local.x > in->settings.width || local.y > in->settings.height) continue;
        if (Widget* w = in->viewport.HitTest(local)) best = {in->entity, w, local, distance};
    }
    if (best.widget == nullptr) best.distance = 0.0f;
    return best;
}

WorldUISystem::Instance* WorldUISystem::InteractiveAt(Vec2 pixel, Vec2& local) {
    Instance* best = nullptr;
    f32 best_distance = std::numeric_limits<f32>::max();
    for (auto& [i, in] : instances_) {
        if (!in->placement.shown || !in->settings.interactive) continue;
        Vec2 l;
        f32 d = 0.0f;
        if (!ToLocal(*in, pixel, l, d) || d >= best_distance) continue;
        if (l.x < 0.0f || l.y < 0.0f || l.x > in->settings.width || l.y > in->settings.height || in->viewport.HitTest(l) == nullptr) continue;
        best = in.get();
        best_distance = d;
        local = l;
    }
    return best;
}

bool WorldUISystem::PointerMove(Vec2 pixel) {
    constexpr f32 kAway = -1e9f;
    if (Instance* in = Find(captured_)) {
        Vec2 local{kAway, kAway};
        f32 d = 0.0f;
        if (in->placement.shown) (void)ToLocal(*in, pixel, local, d);
        in->router->PointerMove(local);
        return true;
    }
    Vec2 local;
    Instance* in = InteractiveAt(pixel, local);
    const Entity now = in != nullptr ? in->entity : kNullEntity;
    if (now != hovered_) {
        if (Instance* old = Find(hovered_)) old->router->PointerMove({kAway, kAway}); // unhover
        hovered_ = now;
    }
    return in != nullptr && in->router->PointerMove(local);
}

bool WorldUISystem::PointerDown(Vec2 pixel, int button) {
    Vec2 local;
    Instance* in = InteractiveAt(pixel, local);
    if (in == nullptr) return false;
    if (hovered_ != in->entity) (void)PointerMove(pixel);
    const bool used = in->router->PointerDown(local, button);
    if (used) captured_ = in->entity;
    return used;
}

bool WorldUISystem::PointerUp(Vec2 pixel, int button) {
    Instance* in = Find(captured_);
    captured_ = kNullEntity;
    if (in == nullptr) return false;
    Vec2 local{-1e9f, -1e9f};
    f32 d = 0.0f;
    if (in->placement.shown) (void)ToLocal(*in, pixel, local, d);
    in->router->PointerUp(local, button);
    return true;
}

} // namespace aether::ui
