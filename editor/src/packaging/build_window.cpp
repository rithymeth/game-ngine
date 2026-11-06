#include "packaging/build_window.h"

#include "ui/reflected_inspector.h"

#include <imgui.h>

#include <cstdio>

namespace aether::editor {

namespace stdfs = std::filesystem;

namespace {

constexpr const char* kCompressionNames[] = {"None", "LZ4", "Zstd", "Auto"};

} // namespace

// ---------------------------------------------------------------------------
// Build and Package
// ---------------------------------------------------------------------------
//
// The work is PackageRunner's; this draws its controls, its progress and its
// log into the current ImGui window.

void BuildPackageWindow::Draw() {
    const bool busy = Busy();
    ImGui::TextUnformatted(project_file_.filename().string().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", project_file_.parent_path().string().c_str());
    ImGui::Separator();

    ImGui::BeginDisabled(busy);
    int config = static_cast<int>(configuration);
    if (ImGui::Combo("Configuration", &config, "Debug\0Development\0Shipping\0")) {
        configuration = static_cast<cook::BuildConfiguration>(config);
    }
    int comp = static_cast<int>(compression);
    if (ImGui::Combo("Compression", &comp, kCompressionNames, 4)) compression = static_cast<pak::CompressionPolicy>(comp);
    int quality = static_cast<int>(texture_quality);
    if (ImGui::SliderInt("Texture quality", &quality, 0, 4)) texture_quality = static_cast<u32>(quality);
    ImGui::Checkbox("Fail on cook warnings", &strict_validation);
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s", output_dir.c_str());
    if (ImGui::InputTextWithHint("Output folder", OutputDir().string().c_str(), buffer, sizeof(buffer))) output_dir = buffer;
    std::snprintf(buffer, sizeof(buffer), "%s", player_path.c_str());
    const std::string found = cook::FindPlayerExecutable().string();
    if (ImGui::InputTextWithHint("Player", found.empty() ? "(aether_player not found)" : found.c_str(), buffer, sizeof(buffer))) {
        player_path = buffer;
    }
    ImGui::Checkbox("Encrypt", &encrypt);
    if (encrypt) {
        ImGui::SameLine();
        std::snprintf(buffer, sizeof(buffer), "%s", encryption_key.c_str());
        ImGui::SetNextItemWidth(-90);
        if (ImGui::InputTextWithHint("##key", "64 hex digits", buffer, sizeof(buffer))) encryption_key = buffer;
        ImGui::SameLine();
        if (ImGui::Button("Generate")) encryption_key = pak::PakKey::Generate().ToHex();
    }
    ImGui::EndDisabled();

    if (ImGui::Button("Cook")) StartCook();
    ImGui::SameLine();
    if (ImGui::Button("Package")) StartPackage();
    ImGui::SameLine();
    ImGui::BeginDisabled(!busy);
    if (ImGui::Button("Cancel")) Cancel();
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool can_launch = !busy && !StagedGame().empty();
    ImGui::BeginDisabled(!can_launch);
    if (ImGui::Button(game_.Running() ? "Running..." : "Launch")) Launch();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    std::snprintf(buffer, sizeof(buffer), "%s", launch_args.c_str());
    if (ImGui::InputTextWithHint("##args", "player arguments", buffer, sizeof(buffer))) launch_args = buffer;
    if (!launch_error_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", launch_error_.c_str());

    const Result result = LastResult();
    ImGui::ProgressBar(Progress(), ImVec2(-1, 0), Stage().c_str());
    if (result == Result::Succeeded && !busy) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "Succeeded");
    if (result == Result::Failed) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Failed");

    ImGui::SeparatorText("Log");
    if (ImGui::BeginChild("##build_log", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (const LogLine& line : Log()) {
            if (line.warning) {
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", line.text.c_str());
            } else {
                ImGui::TextUnformatted(line.text.c_str());
            }
        }
        if (busy) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Project Settings
// ---------------------------------------------------------------------------

ProjectSettingsPanel::ProjectSettingsPanel(stdfs::path project_file) : project_file_(std::move(project_file)) {
    Reload();
}

bool ProjectSettingsPanel::Reload(std::string* error) {
    std::string why;
    ProjectSettings loaded;
    if (!LoadProject(project_file_, loaded, &why)) {
        status_ = "Can't load: " + why;
        if (error) *error = why;
        return false;
    }
    settings_ = std::move(loaded);
    dirty_ = false;
    status_.clear();
    return true;
}

bool ProjectSettingsPanel::Save(std::string* error) {
    std::string why;
    if (!SaveProject(project_file_, settings_, &why)) {
        status_ = "Can't save: " + why;
        if (error) *error = why;
        return false;
    }
    dirty_ = false;
    status_ = "Saved";
    return true;
}

void ProjectSettingsPanel::Draw() {
    ImGui::BeginDisabled(!dirty_);
    if (ImGui::Button("Save")) Save();
    ImGui::SameLine();
    if (ImGui::Button("Revert")) Reload();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", project_file_.filename().string().c_str(), dirty_ ? " (modified)" : "");
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(status_.c_str());
    }
    if (const QualityPreset* q = FindQualityPreset(settings_, settings_.default_quality); !q && !settings_.quality_presets.empty()) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "The default quality '%s' isn't one of the presets",
                           settings_.default_quality.c_str());
    }
    ImGui::Separator();
    if (ImGui::BeginChild("##project_settings")) {
        if (InspectObject(settings_, "project_settings").Changed()) dirty_ = true;
    }
    ImGui::EndChild();
}

} // namespace aether::editor
