#include "vfx/vfx_document.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace aether::editor {

using nlohmann::json;
using namespace vfx;

namespace {

bool Fail(std::string* error, const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
}

Emitter BasicEmitter(const std::string& name) {
    Emitter e;
    e.settings.name = name;
    e.spawn.push_back(SpawnRate{});
    e.init.push_back(InitLifetime{});
    e.init.push_back(InitSize{});
    e.init.push_back(InitVelocity{});
    e.render.push_back(SpriteRenderer{});
    return e;
}

std::string Unique(const std::string& base, const std::set<std::string>& taken) {
    if (!taken.count(base)) return base;
    std::string stem = base;
    while (!stem.empty() && stem.back() >= '0' && stem.back() <= '9') stem.pop_back();
    if (stem.empty()) stem = base;
    for (usize n = 1;; ++n) {
        const std::string candidate = stem + std::to_string(n);
        if (!taken.count(candidate)) return candidate;
    }
}

usize StageSize(const Emitter& e, VfxStage s) {
    switch (s) {
    case VfxStage::Spawn: return e.spawn.size();
    case VfxStage::Init: return e.init.size();
    case VfxStage::Update: return e.update.size();
    case VfxStage::Render: return e.render.size();
    }
    return 0;
}

// Moves or erases within whichever stage vector.
template <typename F>
void WithStage(Emitter& e, VfxStage s, F&& f) {
    switch (s) {
    case VfxStage::Spawn: f(e.spawn); break;
    case VfxStage::Init: f(e.init); break;
    case VfxStage::Update: f(e.update); break;
    case VfxStage::Render: f(e.render); break;
    }
}

template <typename Key>
usize Place(std::vector<Key>& keys, Key k) {
    for (usize i = 0; i < keys.size(); ++i) {
        if (std::abs(keys[i].time - k.time) < 1e-3f) {
            keys[i] = k;
            return i;
        }
    }
    const auto at = std::upper_bound(keys.begin(), keys.end(), k.time, [](f32 t, const Key& x) { return t < x.time; });
    const usize index = static_cast<usize>(at - keys.begin());
    keys.insert(at, k);
    return index;
}

} // namespace

const char* StageKey(VfxStage s) {
    switch (s) {
    case VfxStage::Spawn: return "spawn";
    case VfxStage::Init: return "init";
    case VfxStage::Update: return "update";
    case VfxStage::Render: return "render";
    }
    return "";
}

const char* StageLabel(VfxStage s) {
    switch (s) {
    case VfxStage::Spawn: return "Spawn";
    case VfxStage::Init: return "Initialize";
    case VfxStage::Update: return "Update";
    case VfxStage::Render: return "Render";
    }
    return "";
}

std::vector<std::string> StageModuleNames(VfxStage s) {
    switch (s) {
    case VfxStage::Spawn: return SpawnModuleNames();
    case VfxStage::Init: return InitModuleNames();
    case VfxStage::Update: return UpdateModuleNames();
    case VfxStage::Render: return RenderModuleNames();
    }
    return {};
}

ParticleSystemDocument::ParticleSystemDocument() { asset_.emitters.push_back(BasicEmitter("Emitter")); }

ParticleSystemDocument::ParticleSystemDocument(ParticleSystemAsset asset, std::filesystem::path path) : asset_(std::move(asset)), path_(std::move(path)) {}

bool ParticleSystemDocument::Load(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Fail(error, "can't open '" + path.string() + "'");
    std::stringstream ss;
    ss << f.rdbuf();
    ParticleSystemAsset loaded;
    if (!LoadParticleSystem(ss.str(), loaded, error)) return false;
    asset_ = std::move(loaded);
    path_ = path;
    history_.Clear();
    dirty_ = false;
    ++revision_;
    return true;
}

bool ParticleSystemDocument::Save(std::string* error) {
    if (path_.empty()) return Fail(error, "the particle system has no file yet (Save As)");
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (!f) return Fail(error, "can't write '" + path_.string() + "'");
    f << Text();
    if (!f) return Fail(error, "can't write '" + path_.string() + "'");
    dirty_ = false;
    return true;
}

bool ParticleSystemDocument::SaveAs(const std::filesystem::path& path, std::string* error) {
    const std::filesystem::path old = path_;
    path_ = path;
    if (Save(error)) return true;
    path_ = old;
    return false;
}

std::string ParticleSystemDocument::Name() const { return path_.empty() ? std::string("Untitled") : path_.stem().string(); }

void ParticleSystemDocument::Restore(const json& snapshot) {
    ParticleSystemAsset restored;
    if (LoadParticleSystem(snapshot.dump(), restored)) asset_ = std::move(restored);
}

void ParticleSystemDocument::Changed() {
    dirty_ = true;
    ++revision_;
}

void ParticleSystemDocument::Edit(const std::string& label, const std::function<void(ParticleSystemAsset&)>& change, const std::string& merge_key) {
    history_.Record(label, Snapshot(), merge_key);
    change(asset_);
    Changed();
}

bool ParticleSystemDocument::Undo() {
    json restore;
    if (!history_.Undo(Snapshot(), restore)) return false;
    Restore(restore);
    Changed();
    return true;
}

bool ParticleSystemDocument::Redo() {
    json restore;
    if (!history_.Redo(Snapshot(), restore)) return false;
    Restore(restore);
    Changed();
    return true;
}

// --- Emitters ----------------------------------------------------------------------------------------

std::string ParticleSystemDocument::UniqueEmitterName(const std::string& base) const {
    std::set<std::string> taken;
    for (const Emitter& e : asset_.emitters) taken.insert(e.settings.name);
    return Unique(base.empty() ? std::string("Emitter") : base, taken);
}

usize ParticleSystemDocument::AddEmitter(const std::string& name) {
    const std::string fresh = UniqueEmitterName(name);
    Edit("Add emitter", [&](ParticleSystemAsset& a) { a.emitters.push_back(BasicEmitter(fresh)); });
    return asset_.emitters.size() - 1;
}

bool ParticleSystemDocument::RemoveEmitter(usize i) {
    if (i >= asset_.emitters.size()) return false;
    Edit("Remove emitter", [&](ParticleSystemAsset& a) { a.emitters.erase(a.emitters.begin() + static_cast<std::ptrdiff_t>(i)); });
    return true;
}

std::optional<usize> ParticleSystemDocument::DuplicateEmitter(usize i) {
    if (i >= asset_.emitters.size()) return std::nullopt;
    Emitter copy = asset_.emitters[i];
    copy.settings.name = UniqueEmitterName(copy.settings.name);
    Edit("Duplicate emitter", [&](ParticleSystemAsset& a) { a.emitters.insert(a.emitters.begin() + static_cast<std::ptrdiff_t>(i + 1), copy); });
    return i + 1;
}

bool ParticleSystemDocument::RenameEmitter(usize i, const std::string& name, std::string* error) {
    if (i >= asset_.emitters.size()) return Fail(error, "no such emitter");
    const std::string old = asset_.emitters[i].settings.name;
    if (old == name) return true;
    if (name.empty()) return Fail(error, "an emitter needs a name");
    if (asset_.FindEmitter(name) >= 0) return Fail(error, "there's already an emitter called '" + name + "'");
    Edit("Rename emitter", [&](ParticleSystemAsset& a) {
        a.emitters[i].settings.name = name;
        for (Emitter& e : a.emitters) {
            for (SubEmitter& s : e.sub_emitters) {
                if (s.emitter == old) s.emitter = name;
            }
        }
    });
    return true;
}

bool ParticleSystemDocument::MoveEmitter(usize from, usize to) {
    const usize n = asset_.emitters.size();
    if (from >= n || to >= n) return false;
    if (from == to) return true;
    Edit("Move emitter", [&](ParticleSystemAsset& a) {
        Emitter e = std::move(a.emitters[from]);
        a.emitters.erase(a.emitters.begin() + static_cast<std::ptrdiff_t>(from));
        a.emitters.insert(a.emitters.begin() + static_cast<std::ptrdiff_t>(to), std::move(e));
    });
    return true;
}

json ParticleSystemDocument::EmitterSettings(usize i) const {
    if (i >= asset_.emitters.size()) return json::object();
    json j = EmitterToJson(asset_.emitters[i]);
    for (const char* stage : {"spawn", "init", "update", "render", "sub_emitters", "bindings"}) j.erase(stage);
    // The ones saved only when set.
    for (const auto& [k, v] : {std::pair<const char*, json>{"enabled", true}, {"start_delay", 0.0f}, {"warmup", 0.0f}, {"target", "Auto"}}) {
        if (!j.contains(k)) j[k] = v;
    }
    return j;
}

bool ParticleSystemDocument::SetEmitterSetting(usize i, const std::string& key, const json& value, std::string* error, const std::string& merge_key) {
    if (i >= asset_.emitters.size()) return Fail(error, "no such emitter");
    if (key == "name") {
        if (!value.is_string()) return Fail(error, "a name is a string");
        return RenameEmitter(i, value.get<std::string>(), error);
    }
    if (key == "spawn" || key == "init" || key == "update" || key == "render" || key == "sub_emitters" || key == "bindings") {
        return Fail(error, "'" + key + "' isn't a setting");
    }
    json j = EmitterToJson(asset_.emitters[i]);
    j[key] = value;
    Emitter rebuilt;
    if (!EmitterFromJson(j, rebuilt, error)) return false;
    Edit("Set " + key, [&](ParticleSystemAsset& a) { a.emitters[i] = std::move(rebuilt); }, merge_key);
    return true;
}

// --- Modules -----------------------------------------------------------------------------------------

usize ParticleSystemDocument::ModuleCount(usize emitter, VfxStage stage) const {
    return emitter < asset_.emitters.size() ? StageSize(asset_.emitters[emitter], stage) : 0;
}

std::string ParticleSystemDocument::ModuleName(const ModuleRef& r) const {
    const json m = ModuleJson(r);
    return m.value("module", std::string());
}

void ParticleSystemDocument::RemapBindings(Emitter& e, VfxStage stage, const std::function<i64(usize)>& map) {
    const std::string prefix = std::string(StageKey(stage)) + "[";
    std::vector<ParameterBinding> kept;
    for (ParameterBinding b : e.bindings) {
        if (b.field.rfind(prefix, 0) == 0) {
            const usize close = b.field.find(']');
            const std::string digits = close == std::string::npos ? std::string() : b.field.substr(prefix.size(), close - prefix.size());
            if (!digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                const i64 to = map(static_cast<usize>(std::stoul(digits)));
                if (to < 0) continue; // its module went
                b.field = prefix + std::to_string(to) + b.field.substr(close);
            }
        }
        kept.push_back(std::move(b));
    }
    e.bindings = std::move(kept);
}

std::optional<ModuleRef> ParticleSystemDocument::AddModule(usize emitter, VfxStage stage, const std::string& type, std::string* error) {
    if (emitter >= asset_.emitters.size()) return Fail(error, "no such emitter"), std::nullopt;
    bool made = false;
    SpawnModule sm;
    InitModule im;
    UpdateModule um;
    RenderModule rm;
    switch (stage) {
    case VfxStage::Spawn: made = MakeModule(type, sm); break;
    case VfxStage::Init: made = MakeModule(type, im); break;
    case VfxStage::Update: made = MakeModule(type, um); break;
    case VfxStage::Render: made = MakeModule(type, rm); break;
    }
    if (!made) return Fail(error, "there's no " + std::string(StageLabel(stage)) + " module called '" + type + "'"), std::nullopt;
    Edit("Add " + type, [&](ParticleSystemAsset& a) {
        Emitter& e = a.emitters[emitter];
        switch (stage) {
        case VfxStage::Spawn: e.spawn.push_back(sm); break;
        case VfxStage::Init: e.init.push_back(im); break;
        case VfxStage::Update: e.update.push_back(um); break;
        case VfxStage::Render: e.render.push_back(rm); break;
        }
    });
    return ModuleRef{emitter, stage, StageSize(asset_.emitters[emitter], stage) - 1};
}

bool ParticleSystemDocument::RemoveModule(const ModuleRef& r) {
    if (r.emitter >= asset_.emitters.size() || r.index >= StageSize(asset_.emitters[r.emitter], r.stage)) return false;
    Edit("Remove " + ModuleName(r), [&](ParticleSystemAsset& a) {
        Emitter& e = a.emitters[r.emitter];
        WithStage(e, r.stage, [&](auto& v) { v.erase(v.begin() + static_cast<std::ptrdiff_t>(r.index)); });
        RemapBindings(e, r.stage, [&](usize i) -> i64 { return i == r.index ? -1 : static_cast<i64>(i > r.index ? i - 1 : i); });
    });
    return true;
}

std::optional<ModuleRef> ParticleSystemDocument::MoveModule(const ModuleRef& r, usize to) {
    if (r.emitter >= asset_.emitters.size()) return std::nullopt;
    const usize n = StageSize(asset_.emitters[r.emitter], r.stage);
    if (r.index >= n || to >= n) return std::nullopt;
    if (to == r.index) return r;
    Edit("Move " + ModuleName(r), [&](ParticleSystemAsset& a) {
        Emitter& e = a.emitters[r.emitter];
        WithStage(e, r.stage, [&](auto& v) {
            auto m = std::move(v[r.index]);
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(r.index));
            v.insert(v.begin() + static_cast<std::ptrdiff_t>(to), std::move(m));
        });
        RemapBindings(e, r.stage, [&](usize i) -> i64 {
            if (i == r.index) return static_cast<i64>(to);
            if (r.index < to && i > r.index && i <= to) return static_cast<i64>(i - 1);
            if (to < r.index && i >= to && i < r.index) return static_cast<i64>(i + 1);
            return static_cast<i64>(i);
        });
    });
    return ModuleRef{r.emitter, r.stage, to};
}

json ParticleSystemDocument::ModuleJson(const ModuleRef& r) const {
    if (r.emitter >= asset_.emitters.size()) return json::object();
    const json j = EmitterToJson(asset_.emitters[r.emitter]);
    const json& stage = j[StageKey(r.stage)];
    return r.index < stage.size() ? stage[r.index] : json::object();
}

bool ParticleSystemDocument::SetModuleEnabled(const ModuleRef& r, bool enabled) { return SetModuleField(r, "enabled", enabled); }

bool ParticleSystemDocument::SetModuleField(const ModuleRef& r, const std::string& key, const json& value, std::string* error, const std::string& merge_key) {
    if (r.emitter >= asset_.emitters.size()) return Fail(error, "no such emitter");
    if (key == "module") return Fail(error, "a module's type can't change");
    json j = EmitterToJson(asset_.emitters[r.emitter]);
    json& stage = j[StageKey(r.stage)];
    if (r.index >= stage.size()) return Fail(error, "no such module");
    if (stage[r.index].contains(key) ? stage[r.index][key] == value : key == "enabled" && value == true) return true; // no change
    if (!stage[r.index].contains(key) && key != "enabled") return Fail(error, stage[r.index].value("module", std::string()) + " has no field '" + key + "'");
    stage[r.index][key] = value;
    Emitter rebuilt;
    std::string why;
    if (!EmitterFromJson(j, rebuilt, &why)) {
        // "emitter 'A': update[1] (Vortex): center: ..." -> just the field's complaint.
        const usize colon = why.rfind(key + ":");
        return Fail(error, colon != std::string::npos ? why.substr(colon) : why);
    }
    Edit("Set " + key, [&](ParticleSystemAsset& a) { a.emitters[r.emitter] = std::move(rebuilt); }, merge_key);
    return true;
}

// --- Parameters, bindings, sub-emitters ------------------------------------------------------------------

bool ParticleSystemDocument::AddParameter(const std::string& name, const ParameterValue& value, std::string* error) {
    if (name.empty()) return Fail(error, "a parameter needs a name");
    if (asset_.FindParameter(name) != nullptr) return Fail(error, "there's already a parameter '" + name + "'");
    Edit("Add parameter", [&](ParticleSystemAsset& a) { a.parameters.push_back({name, value}); });
    return true;
}

bool ParticleSystemDocument::RemoveParameter(const std::string& name) {
    if (asset_.FindParameter(name) == nullptr) return false;
    Edit("Remove parameter", [&](ParticleSystemAsset& a) {
        std::erase_if(a.parameters, [&](const ParticleParameter& p) { return p.name == name; });
        for (Emitter& e : a.emitters) std::erase_if(e.bindings, [&](const ParameterBinding& b) { return b.parameter == name; });
    });
    return true;
}

bool ParticleSystemDocument::RenameParameter(const std::string& from, const std::string& to, std::string* error) {
    if (asset_.FindParameter(from) == nullptr) return Fail(error, "no parameter '" + from + "'");
    if (to.empty()) return Fail(error, "a parameter needs a name");
    if (from == to) return true;
    if (asset_.FindParameter(to) != nullptr) return Fail(error, "there's already a parameter '" + to + "'");
    Edit("Rename parameter", [&](ParticleSystemAsset& a) {
        for (ParticleParameter& p : a.parameters) {
            if (p.name == from) p.name = to;
        }
        for (Emitter& e : a.emitters) {
            for (ParameterBinding& b : e.bindings) {
                if (b.parameter == from) b.parameter = to;
            }
        }
    });
    return true;
}

bool ParticleSystemDocument::SetParameterDefault(const std::string& name, const ParameterValue& value, const std::string& merge_key) {
    if (asset_.FindParameter(name) == nullptr) return false;
    Edit("Set parameter", [&](ParticleSystemAsset& a) {
        for (ParticleParameter& p : a.parameters) {
            if (p.name == name) p.value = value;
        }
    }, merge_key);
    return true;
}

bool ParticleSystemDocument::AddBinding(usize emitter, const ParameterBinding& b) {
    if (emitter >= asset_.emitters.size()) return false;
    Edit("Add binding", [&](ParticleSystemAsset& a) { a.emitters[emitter].bindings.push_back(b); });
    return true;
}

bool ParticleSystemDocument::RemoveBinding(usize emitter, usize index) {
    if (emitter >= asset_.emitters.size() || index >= asset_.emitters[emitter].bindings.size()) return false;
    Edit("Remove binding", [&](ParticleSystemAsset& a) {
        auto& v = a.emitters[emitter].bindings;
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(index));
    });
    return true;
}

bool ParticleSystemDocument::AddSubEmitter(usize emitter, const SubEmitter& sub) {
    if (emitter >= asset_.emitters.size()) return false;
    Edit("Add sub-emitter", [&](ParticleSystemAsset& a) { a.emitters[emitter].sub_emitters.push_back(sub); });
    return true;
}

bool ParticleSystemDocument::SetSubEmitter(usize emitter, usize index, const SubEmitter& sub) {
    if (emitter >= asset_.emitters.size() || index >= asset_.emitters[emitter].sub_emitters.size()) return false;
    Edit("Edit sub-emitter", [&](ParticleSystemAsset& a) { a.emitters[emitter].sub_emitters[index] = sub; });
    return true;
}

bool ParticleSystemDocument::RemoveSubEmitter(usize emitter, usize index) {
    if (emitter >= asset_.emitters.size() || index >= asset_.emitters[emitter].sub_emitters.size()) return false;
    Edit("Remove sub-emitter", [&](ParticleSystemAsset& a) {
        auto& v = a.emitters[emitter].sub_emitters;
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(index));
    });
    return true;
}

const std::vector<EmitterDiagnostic>& ParticleSystemDocument::Diagnostics() const {
    if (diagnostics_revision_ != revision_) {
        diagnostics_ = ValidateParticleSystem(asset_);
        diagnostics_revision_ = revision_;
    }
    return diagnostics_;
}

usize ParticleSystemDocument::ErrorCount() const {
    const auto& d = Diagnostics();
    return static_cast<usize>(std::count_if(d.begin(), d.end(), [](const EmitterDiagnostic& x) { return x.error; }));
}

// --- Curve and gradient keys ---------------------------------------------------------------------------

usize AddCurveKey(FloatCurve& c, f32 t, f32 v) { return Place(c.keys, FloatCurve::Key{std::clamp(t, 0.0f, 1.0f), v}); }

usize MoveCurveKey(FloatCurve& c, usize key, f32 t, f32 v) {
    if (key >= c.keys.size()) return key;
    c.keys.erase(c.keys.begin() + static_cast<std::ptrdiff_t>(key));
    return AddCurveKey(c, t, v);
}

bool RemoveCurveKey(FloatCurve& c, usize key) {
    if (key >= c.keys.size() || c.keys.size() <= 1) return false;
    c.keys.erase(c.keys.begin() + static_cast<std::ptrdiff_t>(key));
    return true;
}

usize AddColorKey(ColorGradient& g, f32 t, const LinearColor& c) { return Place(g.colors, ColorGradient::ColorKey{std::clamp(t, 0.0f, 1.0f), c.r, c.g, c.b}); }

usize MoveColorKey(ColorGradient& g, usize key, f32 t) {
    if (key >= g.colors.size()) return key;
    ColorGradient::ColorKey k = g.colors[key];
    g.colors.erase(g.colors.begin() + static_cast<std::ptrdiff_t>(key));
    k.time = std::clamp(t, 0.0f, 1.0f);
    return Place(g.colors, k);
}

bool RemoveColorKey(ColorGradient& g, usize key) {
    if (key >= g.colors.size() || g.colors.size() <= 1) return false;
    g.colors.erase(g.colors.begin() + static_cast<std::ptrdiff_t>(key));
    return true;
}

usize AddAlphaKey(ColorGradient& g, f32 t, f32 a) { return Place(g.alphas, ColorGradient::AlphaKey{std::clamp(t, 0.0f, 1.0f), a}); }

bool RemoveAlphaKey(ColorGradient& g, usize key) {
    if (key >= g.alphas.size() || g.alphas.size() <= 1) return false;
    g.alphas.erase(g.alphas.begin() + static_cast<std::ptrdiff_t>(key));
    return true;
}

} // namespace aether::editor
