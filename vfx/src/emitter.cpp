#include "aether/vfx/emitter.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace aether::vfx {

using nlohmann::json;

namespace {

bool Fail(std::string* error, const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
}

constexpr const char* kShapes[] = {"Point", "Sphere", "Hemisphere", "Box", "Cone", "Circle", "Edge"};
constexpr const char* kVelocityModes[] = {"Cone", "Radial", "Direction"};
constexpr const char* kVolumeShapes[] = {"Sphere", "Box"};
constexpr const char* kSpaces[] = {"World", "Local"};
constexpr const char* kBlends[] = {"Alpha", "Additive", "Premultiplied", "Opaque"};
constexpr const char* kSorts[] = {"None", "BackToFront", "FrontToBack", "OldestFirst", "NewestFirst"};
constexpr const char* kFacings[] = {"Camera", "CameraPosition", "Velocity", "FixedAxis", "FixedPlane"};
constexpr const char* kFlipbooks[] = {"OverLife", "Rate", "Random"};
constexpr const char* kOrientations[] = {"Rotation", "AlignVelocity", "FaceCamera"};
constexpr const char* kRibbonFacings[] = {"Camera", "Axis"};
constexpr const char* kRibbonUvs[] = {"Stretch", "Distance"};

// Each module lists its fields once, through a visitor: writing JSON,
// reading it, and checking values all use the same list.
struct Visitor {
    virtual ~Visitor() = default;
    virtual void Bool(const char* key, bool& v) = 0;
    virtual void Float(const char* key, f32& v) = 0;
    virtual void U32(const char* key, u32& v) = 0;
    virtual void Range(const char* key, FloatRange& v) = 0;
    virtual void Vector(const char* key, Vec3& v) = 0;
    virtual void Curve(const char* key, FloatCurve& v) = 0;
    virtual void Gradient(const char* key, ColorGradient& v) = 0;
    virtual void String(const char* key, std::string& v) = 0;
    virtual void Color(const char* key, LinearColor& v) = 0;
    template <typename E, usize N>
    void Enum(const char* key, E& v, const char* const (&names)[N]) {
        u32 i = static_cast<u32>(v);
        EnumIndex(key, i, names, N);
        v = static_cast<E>(i);
    }
    virtual void EnumIndex(const char* key, u32& v, const char* const* names, usize count) = 0;
};

void Fields(SpawnRate& m, Visitor& v) { v.Float("rate", m.rate); }
void Fields(SpawnBurst& m, Visitor& v) {
    v.Float("time", m.time), v.Range("count", m.count), v.U32("cycles", m.cycles), v.Float("interval", m.interval);
}
void Fields(SpawnPerDistance& m, Visitor& v) { v.Float("per_unit", m.per_unit); }
void Fields(InitLifetime& m, Visitor& v) { v.Range("seconds", m.seconds); }
void Fields(InitShape& m, Visitor& v) {
    v.Enum("shape", m.shape, kShapes), v.Float("radius", m.radius), v.Vector("half_extents", m.half_extents), v.Float("thickness", m.thickness);
}
void Fields(InitVelocity& m, Visitor& v) {
    v.Enum("mode", m.mode, kVelocityModes), v.Vector("direction", m.direction), v.Float("cone_angle", m.cone_angle), v.Range("speed", m.speed);
}
void Fields(InitSize& m, Visitor& v) { v.Range("size", m.size); }
void Fields(InitColor& m, Visitor& v) { v.Gradient("color", m.color); }
void Fields(InitRotation& m, Visitor& v) { v.Range("angle", m.angle), v.Range("spin", m.spin); }
void Fields(InheritVelocity& m, Visitor& v) { v.Float("amount", m.amount); }
void Fields(Gravity& m, Visitor& v) { v.Vector("acceleration", m.acceleration); }
void Fields(Drag& m, Visitor& v) { v.Float("coefficient", m.coefficient); }
void Fields(CurlNoiseForce& m, Visitor& v) { v.Float("strength", m.strength), v.Float("frequency", m.frequency), v.Float("scroll", m.scroll); }
void Fields(Vortex& m, Visitor& v) {
    v.Bool("world", m.world), v.Vector("center", m.center), v.Vector("axis", m.axis), v.Float("strength", m.strength), v.Float("pull", m.pull);
}
void Fields(PointAttractor& m, Visitor& v) {
    v.Bool("world", m.world), v.Vector("position", m.position), v.Float("strength", m.strength), v.Float("radius", m.radius),
        v.Float("kill_radius", m.kill_radius);
}
void Fields(KillVolume& m, Visitor& v) {
    v.Bool("world", m.world), v.Enum("shape", m.shape, kVolumeShapes), v.Vector("center", m.center), v.Float("radius", m.radius),
        v.Vector("half_extents", m.half_extents), v.Bool("kill_inside", m.kill_inside);
}
void Fields(CollisionPlane& m, Visitor& v) {
    v.Bool("world", m.world), v.Vector("normal", m.normal), v.Float("offset", m.offset), v.Float("bounce", m.bounce), v.Float("friction", m.friction),
        v.Float("lifetime_loss", m.lifetime_loss), v.Float("radius_scale", m.radius_scale);
}
void Fields(SizeOverLife& m, Visitor& v) { v.Curve("curve", m.curve); }
void Fields(ColorOverLife& m, Visitor& v) { v.Gradient("gradient", m.gradient); }
void Fields(SpeedOverLife& m, Visitor& v) { v.Curve("curve", m.curve); }
void Fields(SpriteRenderer& m, Visitor& v) {
    v.String("material", m.material), v.Enum("blend", m.blend, kBlends), v.Enum("sort", m.sort, kSorts), v.Enum("facing", m.facing, kFacings);
    v.Vector("axis", m.axis), v.Float("aspect", m.aspect), v.Float("stretch", m.stretch);
    v.U32("columns", m.columns), v.U32("rows", m.rows), v.U32("frames", m.frames), v.Enum("flipbook", m.flipbook, kFlipbooks);
    v.Float("cycles", m.cycles), v.Float("fps", m.fps), v.Bool("blend_frames", m.blend_frames), v.Float("soft_fade", m.soft_fade);
    v.Float("camera_offset", m.camera_offset);
}
void Fields(MeshRenderer& m, Visitor& v) {
    v.String("mesh", m.mesh), v.String("material", m.material), v.Enum("orientation", m.orientation, kOrientations), v.Vector("axis", m.axis);
    v.Vector("scale", m.scale), v.Enum("sort", m.sort, kSorts);
}
void Fields(RibbonRenderer& m, Visitor& v) {
    v.String("material", m.material), v.Enum("blend", m.blend, kBlends), v.Enum("facing", m.facing, kRibbonFacings), v.Vector("axis", m.axis);
    v.Float("width_scale", m.width_scale), v.Enum("uv", m.uv, kRibbonUvs), v.Float("tile_length", m.tile_length);
    v.Bool("attach_to_emitter", m.attach_to_emitter);
}
void Fields(LightRenderer& m, Visitor& v) {
    v.Float("radius_scale", m.radius_scale), v.Float("intensity", m.intensity), v.Bool("use_particle_color", m.use_particle_color);
    v.Color("color", m.color), v.U32("every_nth", m.every_nth), v.U32("max_lights", m.max_lights);
}

template <typename T> constexpr const char* NameOf();
#define AETHER_VFX_NAME(T) \
    template <> constexpr const char* NameOf<T>() { return #T; }
AETHER_VFX_NAME(SpawnRate)
AETHER_VFX_NAME(SpawnBurst)
AETHER_VFX_NAME(SpawnPerDistance)
AETHER_VFX_NAME(InitLifetime)
AETHER_VFX_NAME(InitShape)
AETHER_VFX_NAME(InitVelocity)
AETHER_VFX_NAME(InitSize)
AETHER_VFX_NAME(InitColor)
AETHER_VFX_NAME(InitRotation)
AETHER_VFX_NAME(InheritVelocity)
AETHER_VFX_NAME(Gravity)
AETHER_VFX_NAME(Drag)
AETHER_VFX_NAME(CurlNoiseForce)
AETHER_VFX_NAME(Vortex)
AETHER_VFX_NAME(PointAttractor)
AETHER_VFX_NAME(KillVolume)
AETHER_VFX_NAME(CollisionPlane)
AETHER_VFX_NAME(SizeOverLife)
AETHER_VFX_NAME(ColorOverLife)
AETHER_VFX_NAME(SpeedOverLife)
AETHER_VFX_NAME(SpriteRenderer)
AETHER_VFX_NAME(MeshRenderer)
AETHER_VFX_NAME(RibbonRenderer)
AETHER_VFX_NAME(LightRenderer)
#undef AETHER_VFX_NAME

class Writer final : public Visitor {
public:
    json j = json::object();
    void Bool(const char* k, bool& v) override { j[k] = v; }
    void Float(const char* k, f32& v) override { j[k] = v; }
    void U32(const char* k, u32& v) override { j[k] = v; }
    void Range(const char* k, FloatRange& v) override { j[k] = ToJson(v); }
    void Vector(const char* k, Vec3& v) override { j[k] = ToJson(v); }
    void Curve(const char* k, FloatCurve& v) override { j[k] = ToJson(v); }
    void Gradient(const char* k, ColorGradient& v) override { j[k] = ToJson(v); }
    void String(const char* k, std::string& v) override { j[k] = v; }
    void Color(const char* k, LinearColor& v) override { j[k] = ToJson(v); }
    void EnumIndex(const char* k, u32& v, const char* const* names, usize count) override { j[k] = v < count ? names[v] : names[0]; }
};

class Reader final : public Visitor {
public:
    explicit Reader(const json& j) : j_(j) {}
    std::string error;
    void Bool(const char* k, bool& v) override {
        if (const json* x = Get(k)) {
            if (x->is_boolean()) v = x->get<bool>();
            else Bad(k, "true or false");
        }
    }
    void Float(const char* k, f32& v) override {
        if (const json* x = Get(k)) {
            if (x->is_number()) v = x->get<f32>();
            else Bad(k, "a number");
        }
    }
    void U32(const char* k, u32& v) override {
        if (const json* x = Get(k)) {
            if (x->is_number_unsigned() || (x->is_number_integer() && x->get<i64>() >= 0)) v = x->get<u32>();
            else Bad(k, "a whole number, 0 or more");
        }
    }
    void Range(const char* k, FloatRange& v) override { Parse(k, v); }
    void Vector(const char* k, Vec3& v) override { Parse(k, v); }
    void Curve(const char* k, FloatCurve& v) override { Parse(k, v); }
    void Gradient(const char* k, ColorGradient& v) override { Parse(k, v); }
    void Color(const char* k, LinearColor& v) override { Parse(k, v); }
    void String(const char* k, std::string& v) override {
        if (const json* x = Get(k)) {
            if (x->is_string()) v = x->get<std::string>();
            else Bad(k, "a string");
        }
    }
    void EnumIndex(const char* k, u32& v, const char* const* names, usize count) override {
        const json* x = Get(k);
        if (x == nullptr) return;
        const std::string s = x->is_string() ? x->get<std::string>() : std::string();
        for (usize i = 0; i < count; ++i) {
            if (s == names[i]) {
                v = static_cast<u32>(i);
                return;
            }
        }
        if (error.empty()) error = std::string(k) + ": unknown value '" + s + "'";
    }

private:
    const json* Get(const char* k) const {
        const auto it = j_.find(k);
        return it == j_.end() ? nullptr : &*it;
    }
    void Bad(const char* k, const char* what) {
        if (error.empty()) error = std::string(k) + " should be " + what;
    }
    template <typename T>
    void Parse(const char* k, T& v) {
        const json* x = Get(k);
        if (x == nullptr) return;
        std::string e;
        if (!FromJson(*x, v, &e) && error.empty()) error = std::string(k) + ": " + e;
    }
    const json& j_;
};

// Ranges backwards and keys out of order (FX004, FX007).
class Checker final : public Visitor {
public:
    std::vector<std::string> backwards, unsorted;
    void Bool(const char*, bool&) override {}
    void Float(const char*, f32&) override {}
    void U32(const char*, u32&) override {}
    void Vector(const char*, Vec3&) override {}
    void EnumIndex(const char*, u32&, const char* const*, usize) override {}
    void String(const char*, std::string&) override {}
    void Color(const char*, LinearColor&) override {}
    void Range(const char* k, FloatRange& v) override {
        if (v.min > v.max) backwards.push_back(k);
    }
    void Curve(const char* k, FloatCurve& v) override {
        if (!v.Sorted()) unsorted.push_back(k);
    }
    void Gradient(const char* k, ColorGradient& v) override {
        const bool colors = std::is_sorted(v.colors.begin(), v.colors.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        const bool alphas = std::is_sorted(v.alphas.begin(), v.alphas.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        if (!colors || !alphas) unsorted.push_back(k);
    }
};

template <typename Variant>
json ModuleToJson(const Variant& m) {
    return std::visit(
        [](const auto& module) {
            auto copy = module;
            Writer w;
            w.j["module"] = NameOf<std::decay_t<decltype(module)>>();
            if (!copy.enabled) w.j["enabled"] = false;
            Fields(copy, w);
            return w.j;
        },
        m);
}

// Every alternative of a variant, default-made, for lookups by name.
template <typename Variant, usize I = 0>
bool MakeByName(const std::string& name, Variant& out) {
    if constexpr (I < std::variant_size_v<Variant>) {
        using T = std::variant_alternative_t<I, Variant>;
        if (name == NameOf<T>()) {
            out = T{};
            return true;
        }
        return MakeByName<Variant, I + 1>(name, out);
    } else {
        return false;
    }
}

template <typename Variant, usize I = 0>
void NamesOf(std::vector<std::string>& out) {
    if constexpr (I < std::variant_size_v<Variant>) {
        out.push_back(NameOf<std::variant_alternative_t<I, Variant>>());
        NamesOf<Variant, I + 1>(out);
    }
}

template <typename Variant>
bool ModuleFromJson(const json& j, Variant& out, std::string* error) {
    if (!j.is_object()) return Fail(error, "a module is an object");
    const std::string name = j.value("module", std::string());
    Variant m;
    if (!MakeByName(name, m)) return Fail(error, "unknown module '" + name + "'");
    std::string e;
    std::visit(
        [&](auto& module) {
            Reader r(j);
            r.Bool("enabled", module.enabled);
            Fields(module, r);
            e = r.error;
        },
        m);
    if (!e.empty()) return Fail(error, "(" + name + "): " + e);
    out = std::move(m);
    return true;
}

template <typename Variant>
json StageToJson(const std::vector<Variant>& stage) {
    json a = json::array();
    for (const Variant& m : stage) a.push_back(ModuleToJson(m));
    return a;
}

template <typename Variant>
bool StageFromJson(const json& j, const char* key, std::vector<Variant>& out, std::string* error) {
    out.clear();
    if (!j.contains(key)) return true;
    if (!j[key].is_array()) return Fail(error, std::string(key) + " should be a list of modules");
    for (usize i = 0; i < j[key].size(); ++i) {
        Variant m;
        std::string e;
        if (!ModuleFromJson(j[key][i], m, &e)) return Fail(error, std::string(key) + "[" + std::to_string(i) + "] " + e);
        out.push_back(std::move(m));
    }
    return true;
}

template <typename Variant>
void Check(const std::vector<Variant>& stage, const char* key, std::vector<EmitterDiagnostic>& out) {
    for (usize i = 0; i < stage.size(); ++i) {
        std::visit(
            [&](const auto& module) {
                auto copy = module;
                Checker c;
                Fields(copy, c);
                const std::string where = std::string(key) + "[" + std::to_string(i) + "] (" + NameOf<std::decay_t<decltype(module)>>() + ")";
                for (const std::string& f : c.backwards) out.push_back({"FX004", where + ": " + f + " has its min over its max", true});
                for (const std::string& f : c.unsorted) out.push_back({"FX007", where + ": " + f + " has keys out of order", true});
            },
            stage[i]);
    }
}

} // namespace

const char* ModuleName(const SpawnModule& m) {
    return std::visit([](const auto& x) { return NameOf<std::decay_t<decltype(x)>>(); }, m);
}
const char* ModuleName(const InitModule& m) {
    return std::visit([](const auto& x) { return NameOf<std::decay_t<decltype(x)>>(); }, m);
}
const char* ModuleName(const UpdateModule& m) {
    return std::visit([](const auto& x) { return NameOf<std::decay_t<decltype(x)>>(); }, m);
}
const char* ModuleName(const RenderModule& m) {
    return std::visit([](const auto& x) { return NameOf<std::decay_t<decltype(x)>>(); }, m);
}

std::vector<std::string> SpawnModuleNames() {
    std::vector<std::string> out;
    NamesOf<SpawnModule>(out);
    return out;
}
std::vector<std::string> InitModuleNames() {
    std::vector<std::string> out;
    NamesOf<InitModule>(out);
    return out;
}
std::vector<std::string> UpdateModuleNames() {
    std::vector<std::string> out;
    NamesOf<UpdateModule>(out);
    return out;
}

bool MakeModule(const std::string& name, SpawnModule& out) { return MakeByName(name, out); }
bool MakeModule(const std::string& name, InitModule& out) { return MakeByName(name, out); }
bool MakeModule(const std::string& name, UpdateModule& out) { return MakeByName(name, out); }
bool MakeModule(const std::string& name, RenderModule& out) { return MakeByName(name, out); }
std::vector<std::string> RenderModuleNames() {
    std::vector<std::string> out;
    NamesOf<RenderModule>(out);
    return out;
}

json EmitterToJson(const Emitter& e) {
    const EmitterSettings& s = e.settings;
    json j = {{"name", s.name},
              {"max_particles", s.max_particles},
              {"space", kSpaces[static_cast<usize>(s.space)]},
              {"duration", s.duration},
              {"looping", s.looping}};
    if (!s.enabled) j["enabled"] = false;
    if (s.start_delay != 0.0f) j["start_delay"] = s.start_delay;
    if (s.warmup != 0.0f) j["warmup"] = s.warmup;
    j["spawn"] = StageToJson(e.spawn);
    j["init"] = StageToJson(e.init);
    j["update"] = StageToJson(e.update);
    j["render"] = StageToJson(e.render);
    return j;
}

bool EmitterFromJson(const json& j, Emitter& out, std::string* error) {
    if (!j.is_object()) return Fail(error, "an emitter is an object");
    Emitter e;
    EmitterSettings& s = e.settings;
    std::string where;
    try {
        s.name = j.value("name", s.name);
        s.enabled = j.value("enabled", s.enabled);
        s.max_particles = j.value("max_particles", s.max_particles);
        s.duration = j.value("duration", s.duration);
        s.looping = j.value("looping", s.looping);
        s.start_delay = j.value("start_delay", s.start_delay);
        s.warmup = j.value("warmup", s.warmup);
    } catch (const json::exception&) {
        return Fail(error, "emitter '" + s.name + "': a setting has the wrong type");
    }
    if (j.contains("space")) {
        const std::string sp = j["space"].is_string() ? j["space"].get<std::string>() : std::string();
        if (sp == "World") s.space = SimSpace::World;
        else if (sp == "Local") s.space = SimSpace::Local;
        else return Fail(error, "emitter '" + s.name + "': unknown space '" + sp + "'");
    }
    std::string e2;
    if (!StageFromJson(j, "spawn", e.spawn, &e2) || !StageFromJson(j, "init", e.init, &e2) || !StageFromJson(j, "update", e.update, &e2) ||
        !StageFromJson(j, "render", e.render, &e2)) {
        return Fail(error, "emitter '" + s.name + "': " + e2);
    }
    out = std::move(e);
    return true;
}

std::string SaveParticleSystem(const ParticleSystemAsset& asset) {
    json emitters = json::array();
    for (const Emitter& e : asset.emitters) emitters.push_back(EmitterToJson(e));
    json j = {{"version", 1}, {"emitters", emitters}};
    if (!asset.name.empty()) j["name"] = asset.name;
    return j.dump(2);
}

bool LoadParticleSystem(const std::string& text, ParticleSystemAsset& out, std::string* error) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return Fail(error, "not a particle system: malformed JSON");
    if (j.value("version", 1) > 1) return Fail(error, "made by a newer version (" + std::to_string(j.value("version", 1)) + ")");
    ParticleSystemAsset a;
    a.name = j.value("name", std::string());
    if (j.contains("emitters")) {
        if (!j["emitters"].is_array()) return Fail(error, "emitters should be a list");
        for (const json& ej : j["emitters"]) {
            Emitter e;
            if (!EmitterFromJson(ej, e, error)) return false;
            a.emitters.push_back(std::move(e));
        }
    }
    out = std::move(a);
    return true;
}

std::vector<EmitterDiagnostic> ValidateEmitter(const Emitter& e) {
    std::vector<EmitterDiagnostic> out;
    const EmitterSettings& s = e.settings;
    const bool spawns = std::any_of(e.spawn.begin(), e.spawn.end(), [](const SpawnModule& m) {
        return std::visit([](const auto& x) { return x.enabled; }, m);
    });
    if (!spawns) out.push_back({"FX001", "nothing spawns particles: add a SpawnRate, SpawnBurst or SpawnPerDistance", false});
    for (const InitModule& m : e.init) {
        if (const auto* l = std::get_if<InitLifetime>(&m); l != nullptr && l->enabled && (l->seconds.min <= 0.0f || l->seconds.max <= 0.0f)) {
            out.push_back({"FX002", "lifetimes must be more than 0 seconds", true});
        }
    }
    if (s.max_particles == 0 || s.max_particles > 1000000) out.push_back({"FX003", "max_particles must be 1 to 1,000,000", true});
    Check(e.spawn, "spawn", out);
    Check(e.init, "init", out);
    Check(e.update, "update", out);
    Check(e.render, "render", out);
    const bool draws = std::any_of(e.render.begin(), e.render.end(), [](const RenderModule& m) { return std::visit([](const auto& x) { return x.enabled; }, m); });
    if (!draws) out.push_back({"FX008", "nothing draws its particles: add a SpriteRenderer, MeshRenderer, RibbonRenderer or LightRenderer", false});
    for (usize i = 0; i < e.render.size(); ++i) {
        const std::string where = "render[" + std::to_string(i) + "] (" + ModuleName(e.render[i]) + ")";
        if (const auto* sp = std::get_if<SpriteRenderer>(&e.render[i]); sp != nullptr && sp->enabled) {
            if (sp->columns == 0 || sp->rows == 0 || sp->frames > sp->columns * sp->rows) {
                out.push_back({"FX009", where + ": the flipbook needs columns and rows, and no more frames than cells", true});
            }
        } else if (const auto* me = std::get_if<MeshRenderer>(&e.render[i]); me != nullptr && me->enabled && me->mesh.empty()) {
            out.push_back({"FX010", where + ": no mesh", true});
        }
    }
    for (const SpawnModule& m : e.spawn) {
        if (const auto* b = std::get_if<SpawnBurst>(&m); b != nullptr && b->enabled && b->count.max > static_cast<f32>(s.max_particles)) {
            out.push_back({"FX005", "a burst of " + std::to_string(static_cast<u64>(b->count.max)) + " is more than max_particles", false});
        }
    }
    if (s.duration <= 0.0f) out.push_back({"FX006", "the duration must be more than 0 seconds", true});
    return out;
}

} // namespace aether::vfx
