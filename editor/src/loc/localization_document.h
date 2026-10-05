#pragma once

#include "aether/loc/project_sync.h"
#include "aether/loc/string_table.h"

#include <filesystem>
#include <string>
#include <vector>

// A string table (.astrings) open in the Localization dashboard (Phase 29
// step 5, docs/design/PHASE_SPECS.md §29.5): which languages it has and how
// complete each is, what a language is missing, edits to the cells, keys and
// languages, saving back as CSV, .po import and export, and the gather step
// run on the project's content. Edits stay in memory (the document is dirty)
// until Save; there is no undo yet.

namespace aether::editor {

class LocalizationDocument {
public:
    // Reads the table; false (with Errors()) if it can't. The file stays open
    // either way, so what was wrong with it can be read.
    bool Open(const std::filesystem::path& file);
    bool Reload() { return Open(file_); }
    void Close();
    bool IsOpen() const { return !file_.empty(); }
    const std::filesystem::path& File() const { return file_; }
    // Writes the table back (CSV, atomically). Clears Dirty().
    bool Save();
    bool Dirty() const { return dirty_; }
    u64 Revision() const { return revision_; }
    const std::vector<std::string>& Errors() const { return errors_; }

    const loc::StringTable& Table() const { return table_; }
    // The language the keys' source texts are in (default "en").
    const std::string& SourceLanguage() const { return source_language_; }
    void SetSourceLanguage(const std::string& language);

    struct LangStat {
        std::string language;
        usize count = 0; // keys translated
        usize total = 0; // all keys
        f32 percent = 100.0f;
    };
    std::vector<LangStat> Stats() const; // sorted by language
    // Keys with no (or empty) text in `language`, sorted.
    std::vector<std::string> MissingKeys(const std::string& language) const;

    // Edits. An empty text clears the cell. All return false on a bad
    // argument (an empty or duplicate key, an empty language, a missing key).
    bool SetCell(const std::string& language, const std::string& key, const std::string& text);
    bool AddKey(const std::string& key, const std::string& source_text = {});
    bool RemoveKey(const std::string& key);
    bool AddLanguage(const std::string& language);

    // .po files for translators (§29.4). Import takes the language from the
    // file's header, else `language`; returns how many errors it reported.
    bool ImportPo(const std::string& language, const std::string& po_text);
    bool ExportPo(const std::string& language, const std::filesystem::path& file) const;

    // The gather step on the project's content folder. With `check` nothing is
    // written. Without it the document must be saved first (the gather rewrites
    // the file), and is reloaded after. The report is kept for the panel.
    bool Gather(const std::filesystem::path& content_dir, bool check);
    const loc::SyncReport& LastGather() const { return last_gather_; }

private:
    void Touch() {
        dirty_ = true;
        ++revision_;
    }

    std::filesystem::path file_;
    loc::StringTable table_;
    std::string source_language_ = "en";
    bool dirty_ = false;
    u64 revision_ = 1;
    std::vector<std::string> errors_;
    loc::SyncReport last_gather_;
};

} // namespace aether::editor
