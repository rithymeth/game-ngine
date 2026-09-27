#include "aether/renderer/material_instance.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>

namespace aether::mat {

using json = nlohmann::json;

namespace {

constexpr int kInstanceVersion = 1;
constexpr usize kMaxChainDepth = 16;

json VecJson(const Vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }

bool VecFrom(const json& j, Vec4& out) {
    if (j.is_number()) {
        const f32 f = j.get<f32>();
        out = Vec4(f, f, f, f);
        return true;
    }
    if (!j.is_array() || j.empty() || j.size() > 4) return false;
    f32 c[4] = {0, 0, 0, 0};
    for (usize i = 0; i < j.size(); ++i) {
        if (!j[i].is_number()) return false;
        c[i] = j[i].get<f32>();
    }
    out = Vec4(c[0], c[1], c[2], c[3]);
    return true;
}

template <typename List>
auto FindByName(List& list, const std::string& name) -> decltype(&list[0]) {
    for (auto& item : list) {
        if (item.name == name) return &item;
    }
    return nullptr;
}

} // namespace

// --- MaterialInstance ------------------------------------------------------------------

const ParameterOverride* MaterialInstance::Find(const std::string& name) const {
    for (const ParameterOverride& o : overrides) {
        if (o.name == name) return &o;
    }
    return nullptr;
}

void MaterialInstance::SetValue(const std::string& name, const Vec4& value) {
    Clear(name);
    overrides.push_back({name, value, {}, false});
}

void MaterialInstance::SetTexture(const std::string& name, const std::string& texture) {
    Clear(name);
    overrides.push_back({name, {}, texture, true});
}

bool MaterialInstance::Clear(const std::string& name) {
    const auto it = std::remove_if(overrides.begin(), overrides.end(), [&](const ParameterOverride& o) { return o.name == name; });
    const bool had = it != overrides.end();
    overrides.erase(it, overrides.end());
    return had;
}

json InstanceToJson(const MaterialInstance& instance) {
    json params = json::object();
    for (const ParameterOverride& o : instance.overrides) params[o.name] = o.is_texture ? json{{"texture", o.texture}} : VecJson(o.value);
    return {{"$type", "MaterialInstance"}, {"$version", kInstanceVersion}, {"parent", instance.parent}, {"parameters", params}};
}

bool InstanceFromJson(const json& j, MaterialInstance& out, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (!j.is_object() || j.value("$type", "") != "MaterialInstance") return fail("not a material instance file");
    if (j.value("$version", 0) > kInstanceVersion) return fail("saved by a newer version of the engine");
    MaterialInstance instance;
    const json parent = j.value("parent", json());
    if (!parent.is_string() || parent.get<std::string>().empty()) return fail("a material instance needs a parent");
    instance.parent = parent.get<std::string>();
    const json params = j.value("parameters", json::object());
    if (!params.is_object()) return fail("'parameters' must be an object");
    for (const auto& [name, value] : params.items()) {
        ParameterOverride o;
        o.name = name;
        if (value.is_object()) {
            if (!value.value("texture", json()).is_string()) return fail("parameter '" + name + "' has a bad texture");
            o.is_texture = true;
            o.texture = value["texture"].get<std::string>();
        } else if (!VecFrom(value, o.value)) {
            return fail("parameter '" + name + "' has a bad value");
        }
        instance.overrides.push_back(std::move(o));
    }
    out = std::move(instance);
    return true;
}

bool SaveInstance(const MaterialInstance& instance, const std::filesystem::path& path, std::string* error) {
    std::ofstream file(path);
    if (!file) {
        if (error != nullptr) *error = "can't write " + path.string();
        return false;
    }
    file << InstanceToJson(instance).dump(2) << "\n";
    return static_cast<bool>(file);
}

bool LoadInstance(const std::filesystem::path& path, MaterialInstance& out, std::string* error) {
    std::ifstream file(path);
    if (!file) {
        if (error != nullptr) *error = "can't read " + path.string();
        return false;
    }
    const json j = json::parse(file, nullptr, false);
    if (j.is_discarded()) {
        if (error != nullptr) *error = path.string() + " isn't valid JSON";
        return false;
    }
    return InstanceFromJson(j, out, error);
}

// --- Resolution -------------------------------------------------------------------------

const ResolvedParameter* ResolvedParameters::Find(const std::string& name) const { return FindByName(values, name); }

ResolvedParameters ResolveParameters(const Material& material, const std::vector<const MaterialInstance*>& chain,
                                     std::vector<Diagnostic>* diagnostics) {
    ResolvedParameters out;
    for (const Parameter& p : material.parameters) out.values.push_back({p.name, p.type, p.default_value, p.texture, 0});
    auto warn = [&](const char* code, const std::string& name, const std::string& message) {
        if (diagnostics != nullptr) diagnostics->push_back({code, Severity::Warning, 0, name, message});
    };
    for (usize i = 0; i < chain.size(); ++i) {
        for (const ParameterOverride& o : chain[i]->overrides) {
            ResolvedParameter* p = FindByName(out.values, o.name);
            if (p == nullptr) {
                warn("MI001", o.name, "The material has no parameter named '" + o.name + "'; the override is ignored.");
                continue;
            }
            if (o.is_texture != (p->type == PinType::Texture)) {
                warn("MI002", o.name, "'" + o.name + "' is a " + TypeName(p->type) + " parameter; the override is ignored.");
                continue;
            }
            if (o.is_texture) p->texture = o.texture;
            else p->value = o.value;
            p->source = static_cast<u32>(i + 1);
        }
    }
    return out;
}

std::vector<const MaterialInstance*> InstanceChain::Pointers() const {
    std::vector<const MaterialInstance*> out;
    for (const MaterialInstance& i : instances) out.push_back(&i);
    return out;
}

bool LoadInstanceChain(const std::string& path, const AssetReader& read, InstanceChain& out, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    InstanceChain chain;
    std::set<std::string> visited;
    std::string at = path;
    for (usize depth = 0;; ++depth) {
        if (depth > kMaxChainDepth) return fail("MI005: the instance chain from '" + path + "' is more than 16 deep");
        if (!visited.insert(at).second) return fail("MI003: the instance chain loops back to '" + at + "'");
        const std::optional<json> doc = read(at);
        if (!doc) return fail("MI004: can't read '" + at + "'");
        const std::string type = doc->is_object() ? doc->value("$type", "") : "";
        std::string message;
        if (type == "Material") {
            if (!MaterialFromJson(*doc, chain.material, &message)) return fail("MI004: '" + at + "': " + message);
            chain.material_path = at;
            break;
        }
        if (type != "MaterialInstance") return fail("MI004: '" + at + "' isn't a material or a material instance");
        MaterialInstance instance;
        if (!InstanceFromJson(*doc, instance, &message)) return fail("MI004: '" + at + "': " + message);
        at = instance.parent;
        chain.instances.push_back(std::move(instance));
    }
    std::reverse(chain.instances.begin(), chain.instances.end());
    out = std::move(chain);
    return true;
}

std::vector<u8> PackParameters(const ParameterLayout& layout, const ResolvedParameters& values) {
    std::vector<u8> bytes(layout.size, 0);
    for (const ParameterSlot& slot : layout.slots) {
        const ResolvedParameter* p = values.Find(slot.name);
        if (p == nullptr || slot.offset + slot.size > bytes.size()) continue;
        const f32 c[4] = {p->value.x, p->value.y, p->value.z, p->value.w};
        std::memcpy(bytes.data() + slot.offset, c, slot.size);
    }
    return bytes;
}

std::vector<std::string> TextureBindings(const std::vector<TextureSlot>& slots, const ResolvedParameters& values) {
    std::vector<std::string> out;
    for (const TextureSlot& slot : slots) {
        const ResolvedParameter* p = values.Find(slot.name);
        if (out.size() <= slot.slot) out.resize(slot.slot + 1);
        out[slot.slot] = p != nullptr ? p->texture : std::string();
    }
    return out;
}

// --- ParameterBlock -----------------------------------------------------------------------

ParameterBlock::ParameterBlock(ParameterLayout layout, std::vector<TextureSlot> textures, ResolvedParameters base)
    : layout_(std::move(layout)), texture_slots_(std::move(textures)), base_(std::move(base)), current_(base_) {}

ResolvedParameter* ParameterBlock::Find(const std::string& name) { return FindByName(current_.values, name); }

void ParameterBlock::Changed() {
    dirty_ = true;
    ++revision_;
}

bool ParameterBlock::SetScalar(const std::string& name, f32 value) {
    ResolvedParameter* p = Find(name);
    if (p == nullptr || p->type != PinType::Float) return false;
    if (p->value.x == value) return true;
    p->value = Vec4(value, 0, 0, 0);
    Changed();
    return true;
}

bool ParameterBlock::SetVector(const std::string& name, const Vec4& value) {
    ResolvedParameter* p = Find(name);
    if (p == nullptr || p->type == PinType::Texture || p->type == PinType::Float) return false;
    if (p->value.x == value.x && p->value.y == value.y && p->value.z == value.z && p->value.w == value.w) return true;
    p->value = value;
    Changed();
    return true;
}

bool ParameterBlock::SetTexture(const std::string& name, const std::string& texture) {
    ResolvedParameter* p = Find(name);
    if (p == nullptr || p->type != PinType::Texture) return false;
    if (p->texture == texture) return true;
    p->texture = texture;
    Changed();
    return true;
}

bool ParameterBlock::Reset(const std::string& name) {
    ResolvedParameter* p = Find(name);
    const ResolvedParameter* b = base_.Find(name);
    if (p == nullptr || b == nullptr) return false;
    if (p->texture != b->texture || std::memcmp(&p->value, &b->value, sizeof(Vec4)) != 0) {
        *p = *b;
        Changed();
    }
    return true;
}

void ParameterBlock::ResetAll() {
    for (const ResolvedParameter& b : base_.values) Reset(b.name);
}

std::optional<Vec4> ParameterBlock::Value(const std::string& name) const {
    const ResolvedParameter* p = current_.Find(name);
    if (p == nullptr || p->type == PinType::Texture) return std::nullopt;
    return p->value;
}

const std::vector<u8>& ParameterBlock::Buffer() const {
    if (dirty_) {
        buffer_ = PackParameters(layout_, current_);
        textures_ = TextureBindings(texture_slots_, current_);
        dirty_ = false;
    }
    return buffer_;
}

const std::vector<std::string>& ParameterBlock::Textures() const {
    (void)Buffer();
    return textures_;
}

} // namespace aether::mat

namespace aether {

namespace {
MaterialParameterValue& Slot(MaterialParameters& m, const std::string& name) {
    for (MaterialParameterValue& v : m.values) {
        if (v.name == name) return v;
    }
    m.values.push_back({name, {}, {}, false});
    return m.values.back();
}
const MaterialParameterValue* Get(const MaterialParameters& m, const std::string& name) {
    for (const MaterialParameterValue& v : m.values) {
        if (v.name == name) return &v;
    }
    return nullptr;
}
} // namespace

void MaterialParameters::SetScalarParameter(const std::string& name, f32 value) {
    MaterialParameterValue& v = Slot(*this, name);
    v.value = Vec4(value, 0, 0, 0);
    v.texture.clear();
    v.is_texture = false;
    ++revision;
}

void MaterialParameters::SetVectorParameter(const std::string& name, Vec3 value, f32 alpha) {
    MaterialParameterValue& v = Slot(*this, name);
    v.value = Vec4(value.x, value.y, value.z, alpha);
    v.texture.clear();
    v.is_texture = false;
    ++revision;
}

void MaterialParameters::SetTextureParameter(const std::string& name, const std::string& texture) {
    MaterialParameterValue& v = Slot(*this, name);
    v.value = Vec4(0, 0, 0, 0);
    v.texture = texture;
    v.is_texture = true;
    ++revision;
}

f32 MaterialParameters::GetScalarParameter(const std::string& name) const {
    const MaterialParameterValue* v = Get(*this, name);
    return v != nullptr && !v->is_texture ? v->value.x : 0.0f;
}

Vec3 MaterialParameters::GetVectorParameter(const std::string& name) const {
    const MaterialParameterValue* v = Get(*this, name);
    return v != nullptr && !v->is_texture ? Vec3(v->value.x, v->value.y, v->value.z) : Vec3(0, 0, 0);
}

bool MaterialParameters::ClearParameter(const std::string& name) {
    const auto it = std::remove_if(values.begin(), values.end(), [&](const MaterialParameterValue& v) { return v.name == name; });
    if (it == values.end()) return false;
    values.erase(it, values.end());
    ++revision;
    return true;
}

void MaterialParameters::ApplyTo(mat::ParameterBlock& block) const {
    // Only real changes bump the block's revision: reapplying is free.
    std::vector<std::string> names;
    for (const mat::ResolvedParameter& p : block.Current().values) names.push_back(p.name);
    for (const std::string& name : names) {
        const MaterialParameterValue* v = Get(*this, name);
        if (v == nullptr) {
            block.Reset(name);
        } else if (v->is_texture) {
            if (!block.SetTexture(name, v->texture)) block.Reset(name);
        } else if (!block.SetScalar(name, v->value.x) && !block.SetVector(name, v->value)) {
            block.Reset(name);
        }
    }
}

} // namespace aether
