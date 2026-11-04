#pragma once

#include "aether/animation/blend_space.h"
#include "anim/json_history.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace aether::editor {

class BlendSpaceDocument {
public:
    explicit BlendSpaceDocument(anim::BlendSpace space = {}, std::filesystem::path path = {});
    bool Save(std::string* error = nullptr);
    bool SaveAs(const std::filesystem::path& path, std::string* error = nullptr);
    const anim::BlendSpace& Get() const { return space_; }
    bool Dirty() const { return dirty_; }
    const std::filesystem::path& Path() const { return path_; }
    std::string Name() const;
    void Edit(const std::string& label, const std::function<void(anim::BlendSpace&)>& change,
              const std::string& merge_key = {});
    bool Undo();
    bool Redo();
    bool CanUndo() const { return history_.CanUndo(); }
    bool CanRedo() const { return history_.CanRedo(); }
    usize AddSample(const std::string& clip, f32 x, f32 y);
    bool MoveSample(usize index, f32 x, f32 y, u32 divisions = 0, const std::string& merge_key = {});
    bool RemoveSample(usize index);
    const std::vector<anim::BlendDiagnostic>& Diagnostics() const { return diagnostics_; }

private:
    void Changed();
    anim::BlendSpace space_;
    std::filesystem::path path_;
    bool dirty_ = false;
    JsonHistory history_;
    std::vector<anim::BlendDiagnostic> diagnostics_;
};

struct BlendSpaceCanvas {
    f32 x0 = 0, y0 = 0, width = 1, height = 1;
    void ParamToScreen(const anim::BlendSpace& s, f32 px, f32 py, f32& sx, f32& sy) const;
    void ScreenToParam(const anim::BlendSpace& s, f32 sx, f32 sy, f32& px, f32& py) const;
    i32 HitTest(const anim::BlendSpace& s, f32 sx, f32 sy, f32 radius = 8.0f) const;
};

} // namespace aether::editor
