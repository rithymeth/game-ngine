#pragma once

#include "aether/animation/clip.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace aether::anim {

// Montages (Phase 16 step 4, docs/design/PHASE_SPECS.md §16.5): a clip
// played on top of the graph through a named slot (an attack, a reload, an
// emote), split into sections that can chain, loop, or be jumped to.

struct MontageSection {
    std::string name;
    f32 start = 0.0f; // clip time; a section runs to the next section's start
    std::string next; // the section that follows ("" ends the montage; itself loops)
};

struct Montage {
    std::string name;
    std::string clip;
    std::string slot = "Default";
    f32 blend_in = 0.2f, blend_out = 0.2f; // seconds
    std::vector<MontageSection> sections;  // sorted by start (Normalize); none means one section

    i32 FindSection(const std::string& name) const;
    // The section containing a clip time, and where it ends.
    i32 SectionAt(f32 time) const;
    f32 SectionEnd(i32 section, f32 clip_duration) const;
    void Normalize(); // sorts the sections by start
};

// Diagnostics: MN001 no clip; MN002 a section's `next` names no section;
// MN003 two sections with one name or start; MN004 a negative blend time.
std::vector<std::string> ValidateMontage(const Montage& montage);

// .amontage files.
nlohmann::json MontageToJson(const Montage& montage);
bool MontageFromJson(const nlohmann::json& json, Montage& out, std::string* error = nullptr);

} // namespace aether::anim
