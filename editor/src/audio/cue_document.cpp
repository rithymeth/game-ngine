#include "audio/cue_document.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace aether::editor {

using audio::CueNode;
using audio::CueNodeType;
using audio::SoundCue;

SoundCueDocument::SoundCueDocument(SoundCue cue, std::filesystem::path path) : cue_(std::move(cue)), path_(std::move(path)) {}

bool SoundCueDocument::Load(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error != nullptr) *error = "couldn't open " + path.string();
        return false;
    }
    std::stringstream text;
    text << f.rdbuf();
    SoundCue cue;
    if (!audio::LoadCue(text.str(), cue, error)) return false;
    cue_ = std::move(cue);
    path_ = path;
    history_.Clear();
    dirty_ = false;
    Changed();
    return true;
}

bool SoundCueDocument::Save(std::string* error) {
    if (path_.empty()) {
        if (error != nullptr) *error = "the cue has no file yet (Save As)";
        return false;
    }
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (error != nullptr) *error = "couldn't write " + path_.string();
        return false;
    }
    f << audio::SaveCue(cue_);
    dirty_ = !f.good();
    return f.good();
}

bool SoundCueDocument::SaveAs(const std::filesystem::path& path, std::string* error) {
    path_ = path;
    return Save(error);
}

std::string SoundCueDocument::Name() const {
    if (!cue_.name.empty()) return cue_.name;
    return path_.empty() ? std::string("Untitled Cue") : path_.stem().string();
}

void SoundCueDocument::SetSounds(std::vector<std::string> names) {
    sounds_ = std::move(names);
    Changed();
}

void SoundCueDocument::Changed() { ++revision_; }

void SoundCueDocument::Edit(const std::string& label, const std::function<void(SoundCue&)>& change, const std::string& merge_key) {
    const nlohmann::json before = audio::CueToJson(cue_);
    change(cue_);
    if (audio::CueToJson(cue_) == before) return; // nothing changed: no undo step
    history_.Record(label, before, merge_key);
    dirty_ = true;
    Changed();
}

bool SoundCueDocument::Undo() {
    nlohmann::json restore;
    if (!history_.Undo(audio::CueToJson(cue_), restore)) return false;
    audio::CueFromJson(restore, cue_);
    dirty_ = true;
    Changed();
    return true;
}

bool SoundCueDocument::Redo() {
    nlohmann::json restore;
    if (!history_.Redo(audio::CueToJson(cue_), restore)) return false;
    audio::CueFromJson(restore, cue_);
    dirty_ = true;
    Changed();
    return true;
}

const std::vector<audio::CueDiagnostic>& SoundCueDocument::Diagnostics() const {
    if (diagnostics_revision_ != revision_) {
        std::function<bool(const std::string&)> exists;
        if (!sounds_.empty()) exists = [this](const std::string& s) { return std::find(sounds_.begin(), sounds_.end(), s) != sounds_.end(); };
        diagnostics_ = audio::ValidateCue(cue_, exists);
        diagnostics_revision_ = revision_;
    }
    return diagnostics_;
}

usize SoundCueDocument::ErrorCount() const {
    const auto& d = Diagnostics();
    return static_cast<usize>(std::count_if(d.begin(), d.end(), [](const audio::CueDiagnostic& x) { return x.error; }));
}

bool SoundCueDocument::IsMultiInput(CueNodeType t) {
    return t == CueNodeType::Random || t == CueNodeType::Sequence || t == CueNodeType::Concatenator || t == CueNodeType::Mix;
}

u32 SoundCueDocument::AddNode(CueNodeType type, f32 x, f32 y, const std::string& sound) {
    const u32 id = cue_.NextId();
    Edit(std::string("Add ") + audio::CueNodeTypeName(type), [&](SoundCue& c) {
        CueNode n;
        n.id = id;
        n.type = type;
        n.x = x, n.y = y;
        n.sound = sound;
        c.nodes.push_back(std::move(n));
        if (c.Find(c.root) == nullptr) c.root = id;
    });
    return id;
}

void SoundCueDocument::DeleteNodes(const std::vector<u32>& ids) {
    if (ids.empty()) return;
    Edit(ids.size() == 1 ? "Delete node" : "Delete nodes", [&](SoundCue& c) {
        auto doomed = [&](u32 id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
        std::erase_if(c.nodes, [&](const CueNode& n) { return doomed(n.id); });
        for (CueNode& n : c.nodes) {
            for (usize i = n.children.size(); i-- > 0;) {
                if (!doomed(n.children[i])) continue;
                n.children.erase(n.children.begin() + static_cast<std::ptrdiff_t>(i));
                if (i < n.weights.size()) n.weights.erase(n.weights.begin() + static_cast<std::ptrdiff_t>(i));
                if (i < n.input_db.size()) n.input_db.erase(n.input_db.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        if (doomed(c.root)) c.root = 0;
    });
}

bool SoundCueDocument::Reaches(u32 from, u32 target) const {
    std::vector<u32> stack{from};
    std::vector<u32> seen;
    while (!stack.empty()) {
        const u32 id = stack.back();
        stack.pop_back();
        if (id == target) return true;
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);
        if (const CueNode* n = cue_.Find(id)) stack.insert(stack.end(), n->children.begin(), n->children.end());
    }
    return false;
}

bool SoundCueDocument::Connect(u32 child, u32 parent, usize index, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error != nullptr) *error = m;
        return false;
    };
    const CueNode* p = cue_.Find(parent);
    if (p == nullptr || cue_.Find(child) == nullptr) return fail("That node no longer exists.");
    if (p->type == CueNodeType::Wave) return fail("A Wave plays a sound; it has no inputs.");
    if (child == parent || Reaches(child, parent)) return fail("That would make a loop; use a Loop node to repeat.");
    const bool multi = IsMultiInput(p->type);
    if (!multi && index > 0) return fail(std::string(audio::CueNodeTypeName(p->type)) + " takes one input.");
    if (index > p->children.size()) return fail("That input doesn't exist.");
    Edit("Connect", [&](SoundCue& c) {
        CueNode& n = *c.Find(parent);
        if (index == n.children.size()) n.children.push_back(child);
        else n.children[index] = child;
    });
    return true;
}

bool SoundCueDocument::Disconnect(u32 parent, usize index) {
    const CueNode* p = cue_.Find(parent);
    if (p == nullptr || index >= p->children.size()) return false;
    Edit("Break link", [&](SoundCue& c) {
        CueNode& n = *c.Find(parent);
        n.children.erase(n.children.begin() + static_cast<std::ptrdiff_t>(index));
        if (index < n.weights.size()) n.weights.erase(n.weights.begin() + static_cast<std::ptrdiff_t>(index));
        if (index < n.input_db.size()) n.input_db.erase(n.input_db.begin() + static_cast<std::ptrdiff_t>(index));
    });
    return true;
}

bool SoundCueDocument::SetRoot(u32 id) {
    if (id != 0 && cue_.Find(id) == nullptr) return false;
    Edit("Set output", [&](SoundCue& c) { c.root = id; });
    return true;
}

} // namespace aether::editor
