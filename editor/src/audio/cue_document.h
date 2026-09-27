#pragma once

#include "anim/json_history.h"
#include "aether/audio/cue.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace aether::editor {

// An open sound cue (.acue) in the editor (Phase 17 step 6): the cue, its
// file, undo, cached diagnostics, and edits that keep the graph consistent
// (deleting a node unhooks it everywhere; links can't make loops).
class SoundCueDocument {
public:
    SoundCueDocument() = default;
    explicit SoundCueDocument(audio::SoundCue cue, std::filesystem::path path = {});

    bool Load(const std::filesystem::path& path, std::string* error = nullptr);
    bool Save(std::string* error = nullptr);
    bool SaveAs(const std::filesystem::path& path, std::string* error = nullptr);

    const audio::SoundCue& Get() const { return cue_; }
    const std::filesystem::path& Path() const { return path_; }
    std::string Name() const;
    bool Dirty() const { return dirty_; }
    u64 Revision() const { return revision_; }
    // The sounds a Wave can play (the palette, and CU007).
    void SetSounds(std::vector<std::string> names);
    const std::vector<std::string>& Sounds() const { return sounds_; }

    void Edit(const std::string& label, const std::function<void(audio::SoundCue&)>& change, const std::string& merge_key = {});
    bool Undo();
    bool Redo();
    bool CanUndo() const { return history_.CanUndo(); }
    bool CanRedo() const { return history_.CanRedo(); }
    std::string UndoLabel() const { return history_.UndoLabel(); }

    const std::vector<audio::CueDiagnostic>& Diagnostics() const; // cached per revision
    usize ErrorCount() const;

    // A new node; the first one becomes the output's.
    u32 AddNode(audio::CueNodeType type, f32 x, f32 y, const std::string& sound = {});
    // Deletes nodes and every link to them; the output loses them too.
    void DeleteNodes(const std::vector<u32>& ids);
    // Makes `child` input `index` of `parent`: replaces it, or appends when
    // `index` is one past the end (multi-input nodes). Refused (with a
    // reason) for loops, Waves, and single-input nodes past their one input.
    bool Connect(u32 child, u32 parent, usize index, std::string* error = nullptr);
    // Removes input `index` (a multi-input node's later inputs move up, with their weights and volumes).
    bool Disconnect(u32 parent, usize index);
    bool SetRoot(u32 id);

    static bool IsMultiInput(audio::CueNodeType type);

private:
    void Changed();
    bool Reaches(u32 from, u32 target) const; // does `from` lead to `target`?
    audio::SoundCue cue_;
    std::filesystem::path path_;
    JsonHistory history_;
    std::vector<std::string> sounds_;
    bool dirty_ = false;
    u64 revision_ = 1;
    mutable u64 diagnostics_revision_ = 0;
    mutable std::vector<audio::CueDiagnostic> diagnostics_;
};

} // namespace aether::editor
