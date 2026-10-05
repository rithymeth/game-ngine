#pragma once

#include "loc/localization_document.h"

#include "aether/loc/localization.h"

#include <filesystem>
#include <string>

namespace aether::editor {

// The Localization dashboard (Phase 29 step 5, §29.5): each language and how
// complete it is, the keys a language is missing with a box to type the
// translation, adding and removing keys and languages, .po import and export,
// the gather step (checked first by default), and a pseudo-localization
// preview that makes the editor's own UI text show the pseudo language.
class LocalizationPanel {
public:
    explicit LocalizationPanel(LocalizationDocument& document) : doc_(document) {}
    ~LocalizationPanel();

    // The folder the gather step scans and .po files are written to.
    void SetContentDirectory(const std::filesystem::path& dir) { content_dir_ = dir; }
    void SetPoDirectory(const std::filesystem::path& dir) { po_dir_ = dir; }
    const std::filesystem::path& ContentDirectory() const { return content_dir_; }

    const std::string& SelectedLanguage() const { return selected_; }
    void SelectLanguage(const std::string& language) { selected_ = language; }

    // Pseudo-localization preview: makes a Localization holding the table plus
    // the pseudo language the active one (so text with a key shows pseudo text),
    // and puts the previous active one back when switched off. Rebuilt when the
    // table changes.
    void SetPreview(bool on);
    bool Previewing() const { return previewing_; }
    const loc::Localization& PreviewLocalization() const { return preview_; }

    // What Draw's buttons do (callable directly).
    bool ExportSelectedPo();
    bool ImportPoFile(const std::filesystem::path& file);

    void Draw();

private:
    void RefreshPreview();

    LocalizationDocument& doc_;
    std::filesystem::path content_dir_, po_dir_;
    std::string selected_;
    std::string new_key_, new_language_, message_;
    bool check_only_ = true;
    bool previewing_ = false;
    u64 preview_revision_ = 0;
    loc::Localization preview_;
    loc::Localization* previous_active_ = nullptr;
};

} // namespace aether::editor
