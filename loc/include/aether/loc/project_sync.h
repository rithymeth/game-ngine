#pragma once

#include "aether/loc/gather.h"
#include "aether/loc/merge.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aether::loc {

struct SyncOptions {
    std::filesystem::path content_dir;
    std::filesystem::path strings_file; // the project's .astrings (read if it exists, written back)
    std::string source_language = "en";
    std::vector<std::string> languages; // the .po files to write; empty: every language in the table but the source's
    std::filesystem::path po_out;       // a folder for <language>.po files (none if empty)
    std::filesystem::path po_in;        // a .po file, or a folder of them, applied before the merge (none if empty)
    bool write = true;                  // false: report only
};

struct SyncReport {
    bool ok = false;
    GatherReport gathered;
    MergeResult merge;
    usize po_imported = 0; // .po files applied
    usize po_written = 0;
    std::vector<std::string> messages; // warnings and the error that stopped it, one line each
};

// What `aether_loc` does (§29.4): gathers the project's strings, applies any
// returned .po files, merges the keys into the string table (new keys with
// their source text; no translation touched; unused keys reported), writes
// the table back as CSV and the .po files out. Deterministic, so a second run
// with nothing changed rewrites identical files.
SyncReport SyncProject(const SyncOptions& options);

} // namespace aether::loc
