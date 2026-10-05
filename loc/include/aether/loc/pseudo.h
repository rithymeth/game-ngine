#pragma once

#include "aether/loc/string_table.h"

#include <string>
#include <string_view>

namespace aether::loc {

// Pseudo-localization (Phase 29 step 5, §29.5): text that looks like a
// translation without being one, to find what a real one would break before
// there is one: accented letters (so a font missing them shows up), about a
// third longer (so a layout that only fits English shows up), wrapped in
// [ ] (so a clipped end shows up, and so text that skipped the string tables
// stands out as having no brackets).
struct PseudoOptions {
    f32 expansion = 0.35f; // extra length, as a fraction of the text's visible length
    bool accents = true;
    bool brackets = true;
};

// `{...}` argument spans, `{{` / `}}` and line breaks are copied as they are, so a
// pseudo string still formats. An unclosed `{` is copied literally. Deterministic.
std::string Pseudo(std::string_view text, const PseudoOptions& options = {});

// The language tag the editor previews with ("qps-Ploc", the BCP 47 tag
// reserved for pseudo-localization).
const std::string& PseudoLanguage();

// A copy of `table` with `PseudoLanguage()` added: for each key with text in
// `source_language`, its pseudo form.
StringTable PseudoTable(const StringTable& table, std::string_view source_language, const PseudoOptions& options = {});

} // namespace aether::loc
