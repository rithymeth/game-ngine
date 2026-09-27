#pragma once

#include "anim/json_history.h"
#include "aether/vfx/emitter.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aether::editor {

// An emitter's four module stacks.
enum class VfxStage : u8 { Spawn, Init, Update, Render };
const char* StageKey(VfxStage stage);  // "spawn", "init", "update", "render" (as in the file and binding paths)
const char* StageLabel(VfxStage stage); // "Spawn", "Initialize", ...
std::vector<std::string> StageModuleNames(VfxStage stage);

struct ModuleRef {
    usize emitter = 0;
    VfxStage stage = VfxStage::Spawn;
    usize index = 0;
    bool operator==(const ModuleRef& o) const { return emitter == o.emitter && stage == o.stage && index == o.index; }
};

// An open particle system (.avfx) in the particle editor (Phase 19 step 6):
// the asset, its file, whole-document undo, cached checks, and edits that
// keep it consistent (emitter names that sub-emitters follow, module
// moves that binding paths follow, parameter renames that bindings follow).
class ParticleSystemDocument {
public:
    ParticleSystemDocument(); // one emitter with a basic stack
    explicit ParticleSystemDocument(vfx::ParticleSystemAsset asset, std::filesystem::path path = {});

    bool Load(const std::filesystem::path& path, std::string* error = nullptr);
    bool Save(std::string* error = nullptr);
    bool SaveAs(const std::filesystem::path& path, std::string* error = nullptr);
    std::string Text() const { return vfx::SaveParticleSystem(asset_); }

    const vfx::ParticleSystemAsset& Get() const { return asset_; }
    const vfx::Emitter* EmitterAt(usize i) const { return i < asset_.emitters.size() ? &asset_.emitters[i] : nullptr; }
    const std::filesystem::path& Path() const { return path_; }
    std::string Name() const;
    bool Dirty() const { return dirty_; }
    u64 Revision() const { return revision_; }

    void Edit(const std::string& label, const std::function<void(vfx::ParticleSystemAsset&)>& change, const std::string& merge_key = {});
    bool Undo();
    bool Redo();
    bool CanUndo() const { return history_.CanUndo(); }
    bool CanRedo() const { return history_.CanRedo(); }
    std::string UndoLabel() const { return history_.UndoLabel(); }

    // --- Emitters -----------------------------------------------------------------------
    usize AddEmitter(const std::string& name = "Emitter"); // made unique; with a basic stack (rate, lifetime, size, velocity, sprites)
    bool RemoveEmitter(usize i);
    std::optional<usize> DuplicateEmitter(usize i);
    // Unique names; sub-emitters that named it follow.
    bool RenameEmitter(usize i, const std::string& name, std::string* error = nullptr);
    bool MoveEmitter(usize from, usize to);
    // A setting by its saved key ("max_particles", "space", "looping", ...).
    nlohmann::json EmitterSettings(usize i) const;
    bool SetEmitterSetting(usize i, const std::string& key, const nlohmann::json& value, std::string* error = nullptr, const std::string& merge_key = {});
    std::string UniqueEmitterName(const std::string& base) const;

    // --- Modules ------------------------------------------------------------------------
    std::optional<ModuleRef> AddModule(usize emitter, VfxStage stage, const std::string& type, std::string* error = nullptr);
    bool RemoveModule(const ModuleRef& ref);          // bindings to it go
    std::optional<ModuleRef> MoveModule(const ModuleRef& ref, usize to); // bindings follow
    bool SetModuleEnabled(const ModuleRef& ref, bool enabled);
    // The module as saved ({"module": "Gravity", "acceleration": [...], ...}).
    nlohmann::json ModuleJson(const ModuleRef& ref) const;
    bool SetModuleField(const ModuleRef& ref, const std::string& key, const nlohmann::json& value, std::string* error = nullptr,
                        const std::string& merge_key = {});
    std::string ModuleName(const ModuleRef& ref) const;
    usize ModuleCount(usize emitter, VfxStage stage) const;

    // --- Parameters, bindings, sub-emitters ----------------------------------------------
    bool AddParameter(const std::string& name, const vfx::ParameterValue& value, std::string* error = nullptr);
    bool RemoveParameter(const std::string& name); // bindings to it go
    bool RenameParameter(const std::string& from, const std::string& to, std::string* error = nullptr);
    bool SetParameterDefault(const std::string& name, const vfx::ParameterValue& value, const std::string& merge_key = {});
    bool AddBinding(usize emitter, const vfx::ParameterBinding& binding);
    bool RemoveBinding(usize emitter, usize index);
    bool AddSubEmitter(usize emitter, const vfx::SubEmitter& sub);
    bool SetSubEmitter(usize emitter, usize index, const vfx::SubEmitter& sub);
    bool RemoveSubEmitter(usize emitter, usize index);

    const std::vector<vfx::EmitterDiagnostic>& Diagnostics() const; // ValidateParticleSystem, per revision
    usize ErrorCount() const;

private:
    nlohmann::json Snapshot() const { return nlohmann::json::parse(Text()); }
    void Restore(const nlohmann::json& snapshot);
    void Changed();
    // Rewrites the emitter's bindings on `stage` through `map` (old index -> new, -1: dropped).
    static void RemapBindings(vfx::Emitter& e, VfxStage stage, const std::function<i64(usize)>& map);

    vfx::ParticleSystemAsset asset_;
    std::filesystem::path path_;
    JsonHistory history_;
    bool dirty_ = false;
    u64 revision_ = 1;
    mutable u64 diagnostics_revision_ = 0;
    mutable std::vector<vfx::EmitterDiagnostic> diagnostics_;
};

// Key editing for curves and gradients (the details panel's editors).
// Times are clamped to 0..1 and keys kept in order; a key at an existing
// key's time (within 0.001) replaces it; the last key can't be removed.
usize AddCurveKey(vfx::FloatCurve& curve, f32 time, f32 value);
usize MoveCurveKey(vfx::FloatCurve& curve, usize key, f32 time, f32 value); // its new index
bool RemoveCurveKey(vfx::FloatCurve& curve, usize key);
usize AddColorKey(vfx::ColorGradient& gradient, f32 time, const vfx::LinearColor& color);
usize MoveColorKey(vfx::ColorGradient& gradient, usize key, f32 time);
bool RemoveColorKey(vfx::ColorGradient& gradient, usize key);
usize AddAlphaKey(vfx::ColorGradient& gradient, f32 time, f32 alpha);
bool RemoveAlphaKey(vfx::ColorGradient& gradient, usize key);

} // namespace aether::editor
