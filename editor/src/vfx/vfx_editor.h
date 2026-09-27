#pragma once

#include "vfx/vfx_document.h"
#include "vfx/vfx_preview.h"

#include <optional>
#include <string>

namespace aether::editor {

// The particle editor (Phase 19 step 6, ROADMAP.md Phase 19):
//   Emitters   the system's emitters (add, duplicate, remove, reorder, enable);
//   Stack      the selected emitter's Spawn, Initialize, Update and Render
//              modules (add from each stage's list, enable, reorder, remove);
//   Details    the selected module's fields (curves and gradients with
//              their own editors, ranges as min/max) or the emitter's
//              settings, sub-emitters and bindings;
//   Preview    the system playing, with an orbit camera, play/pause, speed,
//              looping and a timeline scrubber (deterministic replay);
//   Parameters, Stats (per emitter: particles, spawned, bounds, CPU time,
//   CPU or GPU) and Diagnostics.
// It fills the current ImGui window and runs headless in tests.
class ParticleEditor {
public:
    explicit ParticleEditor(ParticleSystemDocument& document);

    void Draw();

    i64 SelectedEmitter() const { return emitter_; }
    void SelectEmitter(usize index);
    std::optional<ModuleRef> SelectedModule() const { return module_; }
    void SelectModule(const ModuleRef& ref);
    void ClearModule() { module_.reset(); }

    ParticlePreview& Preview() { return preview_; }
    bool Playing() const { return playing_; }
    void Play() { playing_ = true; }
    void Pause() { playing_ = false; }
    // Scrubs the preview to a time (replaying from the start).
    void SetTime(f32 time);
    f32 Time() const { return preview_.Time(); }
    f32 speed = 1.0f;
    bool loop = true;
    f32 loop_length = 5.0f; // the preview starts over after this long

    bool SaveNow();
    const std::string& Status() const { return status_; }

private:
    void Sync(); // the preview follows the document
    void DrawToolbar();
    void DrawEmitters();
    void DrawStack();
    void DrawDetails();
    void DrawModuleDetails(const ModuleRef& ref);
    void DrawEmitterDetails(usize emitter);
    void DrawPreview();
    void DrawParameters();
    void DrawStats();
    void DrawDiagnostics();
    void HandleKeys();

    ParticleSystemDocument& doc_;
    ParticlePreview preview_;
    u64 preview_revision_ = 0;
    i64 emitter_ = 0;
    std::optional<ModuleRef> module_;
    bool playing_ = true;
    std::string name_edit_;
    std::string new_parameter_ = "Intensity";
    int new_parameter_type_ = 0;
    std::string binding_field_ = "spawn[0].rate";
    int binding_param_ = 0;
    std::string status_;
};

} // namespace aether::editor
