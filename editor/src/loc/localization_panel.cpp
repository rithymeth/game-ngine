#include "localization_panel.h"

#include "aether/loc/language.h"
#include "aether/loc/pseudo.h"
#include "aether/platform/filesystem.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

namespace aether::editor {

namespace stdfs = std::filesystem;

LocalizationPanel::~LocalizationPanel() {
    if (previewing_) SetPreview(false);
}

void LocalizationPanel::SetPreview(bool on) {
    if (on == previewing_) return;
    previewing_ = on;
    if (on) {
        previous_active_ = loc::Localization::Active();
        RefreshPreview();
        preview_.MakeActive();
    } else {
        if (loc::Localization::Active() == &preview_) {
            if (previous_active_) previous_active_->MakeActive();
            else loc::Localization::ClearActive();
        }
        previous_active_ = nullptr;
    }
}

void LocalizationPanel::RefreshPreview() {
    preview_.Table() = loc::PseudoTable(doc_.Table(), doc_.SourceLanguage());
    preview_.SetLanguage(loc::PseudoLanguage());
    preview_revision_ = doc_.Revision();
}

bool LocalizationPanel::ExportSelectedPo() {
    if (selected_.empty() || po_dir_.empty()) return false;
    const bool ok = doc_.ExportPo(selected_, po_dir_ / (selected_ + ".po"));
    message_ = ok ? "Wrote " + (po_dir_ / (selected_ + ".po")).string() : "Couldn't write the .po file";
    return ok;
}

bool LocalizationPanel::ImportPoFile(const stdfs::path& file) {
    std::string text;
    if (!fs::ReadFileText(file.string(), text)) {
        message_ = "Couldn't read " + file.string();
        return false;
    }
    const bool ok = doc_.ImportPo(selected_, text);
    message_ = ok ? "Imported " + file.filename().string() : "The import reported problems";
    return ok;
}

void LocalizationPanel::Draw() {
    if (previewing_ && preview_revision_ != doc_.Revision()) RefreshPreview();
    if (!doc_.IsOpen()) {
        ImGui::TextDisabled("No string table open (a .astrings file).");
        return;
    }
    ImGui::Text("%s%s", doc_.File().filename().string().c_str(), doc_.Dirty() ? " *" : "");
    ImGui::SameLine();
    if (ImGui::Button("Save")) message_ = doc_.Save() ? "Saved." : "Couldn't save.";
    ImGui::SameLine();
    if (ImGui::Button("Reload")) doc_.Reload();
    ImGui::SameLine();
    bool preview = previewing_;
    if (ImGui::Checkbox("Pseudo-localization preview", &preview)) SetPreview(preview);
    if (!message_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", message_.c_str());
    }
    for (const std::string& e : doc_.Errors()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "! %s", e.c_str());
    ImGui::Separator();

    if (ImGui::BeginTable("##langs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Language");
        ImGui::TableSetupColumn("Translated");
        ImGui::TableSetupColumn("Complete");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const LocalizationDocument::LangStat& s : doc_.Stats()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool is_source = s.language == doc_.SourceLanguage();
            ImGui::Text("%s%s", s.language.c_str(), is_source ? " (source)" : "");
            ImGui::TableNextColumn();
            ImGui::Text("%zu / %zu", s.count, s.total);
            ImGui::TableNextColumn();
            ImGui::ProgressBar(s.percent / 100.0f, ImVec2(-1, 0));
            ImGui::TableNextColumn();
            ImGui::PushID(s.language.c_str());
            if (ImGui::Selectable("Edit", selected_ == s.language)) selected_ = s.language;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::InputText("New language", &new_language_);
    ImGui::SameLine();
    if (ImGui::Button("Add language") && doc_.AddLanguage(new_language_)) {
        selected_ = loc::NormalizeLanguage(new_language_);
        new_language_.clear();
    }
    ImGui::Separator();

    if (selected_.empty()) {
        ImGui::TextDisabled("Pick a language to see what it is missing.");
    } else {
        const std::vector<std::string> missing = doc_.MissingKeys(selected_);
        ImGui::Text("%s: %zu missing", selected_.c_str(), missing.size());
        ImGui::SameLine();
        if (ImGui::Button("Export .po")) ExportSelectedPo();
        if (ImGui::BeginChild("##missing", ImVec2(0, 220), ImGuiChildFlags_Borders)) {
            usize shown = 0;
            for (const std::string& key : missing) {
                if (++shown > 200) { // a long list can't stall a frame
                    ImGui::TextDisabled("... and %zu more", missing.size() - 200);
                    break;
                }
                const std::string* source = doc_.Table().Find(doc_.SourceLanguage(), key);
                ImGui::PushID(key.c_str());
                ImGui::TextUnformatted(key.c_str());
                if (source) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", source->c_str());
                }
                std::string text;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##t", &text, ImGuiInputTextFlags_EnterReturnsTrue)) doc_.SetCell(selected_, key, text);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    ImGui::InputText("New key", &new_key_);
    ImGui::SameLine();
    if (ImGui::Button("Add key") && doc_.AddKey(new_key_)) new_key_.clear();
    ImGui::Separator();

    ImGui::Checkbox("Check only", &check_only_);
    ImGui::SameLine();
    if (ImGui::Button("Gather from project") && !content_dir_.empty()) {
        const bool ok = doc_.Gather(content_dir_, check_only_);
        message_ = ok ? "Gathered." : "The gather didn't run (see the messages).";
    }
    const loc::SyncReport& g = doc_.LastGather();
    if (g.ok) {
        ImGui::Text("%zu string(s) found, %zu new, %zu changed, %zu unused", g.gathered.entries.size(), g.merge.added, g.merge.updated_source,
                    g.merge.unused.size());
    }
    for (const std::string& m : g.messages) ImGui::TextDisabled("%s", m.c_str());
}

} // namespace aether::editor
