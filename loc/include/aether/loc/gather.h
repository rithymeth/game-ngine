#pragma once

#include "aether/core/base.h"
#include "aether/reflection/serialize.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aether::loc {

// One localizable string found in the project's content (Phase 29 step 4, §29.4).
struct GatheredText {
    std::string key;
    std::string source; // the text as written: the source language's string
    std::string where;  // "Scenes/main.ascene: /entities/3/..." for the translator and the log
};

struct GatherReport {
    std::vector<GatheredText> entries; // one per key, sorted by key
    std::vector<std::string> warnings;
};

// Walks one parsed JSON document (a scene, prefab, UI layout or Blueprint) and
// adds what it finds to `report` (not yet merged by key; GatherContent does that):
//  - an object with a non-empty `text_key` (a Text widget; source: its `text`)
//    or `hint_key` (a text box; source: its `hint`);
//  - a reflected LocText: an object with string `key` and `source` and nothing
//    else but `$v`, with a non-empty key;
//  - a Blueprint node of type Call.Native:Localize.GetText / FormatInt /
//    FormatString or Call.Native:UI.SetTextKey whose `key` pin has a literal
//    default (source: its `default`). A key that comes from a linked pin can't be
//    known here, so it is a warning.
void GatherJson(const reflect::Json& document, const std::string& where, GatherReport& report);

// Gathers every .ascene, .aprefab, .aui and .abp under `content_dir`, merged by
// key. The same key with two different source texts is a warning (the first,
// by path, is kept). Unreadable or malformed files are warnings.
GatherReport GatherContent(const std::filesystem::path& content_dir);

} // namespace aether::loc
