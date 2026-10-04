#include "packaging/build_window.h"

#include "ui/reflected_inspector.h"

#include <imgui.h>

#include <cstdio>
#include <sstream>

namespace aether::editor {

namespace stdfs = std::filesystem;

namespace {

constexpr const char* kCompressionNames[] = {"None", "LZ4", "Zstd", "Auto"};

std::vector<std::string> SplitArgs(const std::string& text) {
    std::vector<std::string> args;
    std::istringstream in(text);
    for (std::string a; in >> a;) args.push_back(a);
    return args;
}

} // namespace

// ---------------------------------------------------------------------------
// Build and Package
// ---------------------------------------------------------------------------

BuildPackageWindow::BuildPackageWindow(stdfs::path project_file) : project_file_(std::move(project_file)) {}

BuildPackageWindow::~BuildPackageWindow() {
    Cancel();
    WaitForBuild();
}

stdfs::path BuildPackageWindow::OutputDir() const {
    if (!output_dir.empty()) return output_dir;
    return ProjectPaths::ForFile(project_file_).saved / "Packaged" / cook::ConfigurationName(configuration);
}

void BuildPackageWindow::AddLog(bool warning, std::string text) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_.push_back({warning, std::move(text)});
}

bool BuildPackageWindow::StartCook() { return Start(false); }
bool BuildPackageWindow::StartPackage() { return Start(true); }

bool BuildPackageWindow::Start(bool package) {
    if (busy_.load()) return false;
    WaitForBuild(); // the last one's thread
    {
        std::lock_guard<std::mutex> lock(mutex_);
        log_.clear();
        progress_ = 0.0f;
        stage_ = package ? "Packaging" : "Cooking";
        result_ = Result::None;
    }
    cancel_ = false;
    busy_ = true;

    cook::CookOptions cook_options;
    cook_options.project_file = project_file_;
    cook_options.configuration = configuration;
    cook_options.compression = compression;
    cook_options.texture_quality = texture_quality;
    cook_options.cancel = &cancel_;
    cook_options.progress = [this](f32 fraction, const std::string& stage) {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_ = fraction;
        stage_ = stage;
        log_.push_back({false, stage});
    };
    const stdfs::path out = OutputDir();
    const std::string player = player_path;
    worker_ = std::thread([this, package, cook_options, out, player]() mutable {
        bool ok = false;
        std::string error;
        cook::CookReport report;
        stdfs::path game;
        if (package) {
            cook::PackageOptions options;
            options.cook = std::move(cook_options);
            options.output_dir = out;
            options.player_executable = player;
            cook::PackageReport packaged = cook::Package(options);
            ok = packaged.ok;
            error = packaged.error;
            report = std::move(packaged.cook);
            game = packaged.game_executable;
        } else {
            cook_options.output_dir =
                ProjectPaths::ForFile(project_file_).saved / "Cooked" / cook::ConfigurationName(cook_options.configuration);
            report = cook::Cook(cook_options);
            ok = report.ok;
            error = report.error;
        }
        for (const std::string& w : report.warnings) AddLog(true, w);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ok) {
                result_ = Result::Succeeded;
                progress_ = 1.0f;
                stage_ = package ? "Packaged into " + out.string() : "Cooked into " + report.pak_file.string();
                log_.push_back({false, stage_ + " (" + std::to_string(report.assets.size()) + " assets, " +
                                           std::to_string(report.pak_bytes / 1024) + " KB)"});
                if (package) staged_game_ = game;
            } else {
                result_ = cancel_.load() && error == "Cancelled" ? Result::Cancelled : Result::Failed;
                stage_ = result_ == Result::Cancelled ? "Cancelled" : "Failed: " + error;
                log_.push_back({true, stage_});
            }
        }
        busy_ = false;
    });
    return true;
}

void BuildPackageWindow::Cancel() { cancel_ = true; }

void BuildPackageWindow::WaitForBuild() {
    if (worker_.joinable()) worker_.join();
}

f32 BuildPackageWindow::Progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

std::string BuildPackageWindow::Stage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stage_;
}

std::vector<BuildPackageWindow::LogLine> BuildPackageWindow::Log() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_;
}

BuildPackageWindow::Result BuildPackageWindow::LastResult() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

stdfs::path BuildPackageWindow::StagedGame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return staged_game_;
}

bool BuildPackageWindow::Launch(std::string* error) {
    const stdfs::path game = StagedGame();
    std::string why;
    bool ok = false;
    if (game.empty()) {
        why = "Package the game first";
    } else if (game_.Running()) {
        why = "The game is already running";
    } else {
        ok = game_.Start(game, SplitArgs(launch_args), game.parent_path(), &why);
    }
    launch_error_ = ok ? std::string() : why;
    if (!ok && error) *error = why;
    if (ok) AddLog(false, "Launched " + game.filename().string());
    return ok;
}

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
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s", output_dir.c_str());
    if (ImGui::InputTextWithHint("Output folder", OutputDir().string().c_str(), buffer, sizeof(buffer))) output_dir = buffer;
    std::snprintf(buffer, sizeof(buffer), "%s", player_path.c_str());
    const std::string found = cook::FindPlayerExecutable().string();
    if (ImGui::InputTextWithHint("Player", found.empty() ? "(aether_player not found)" : found.c_str(), buffer, sizeof(buffer))) {
        player_path = buffer;
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
