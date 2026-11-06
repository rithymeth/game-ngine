#include "core/package_runner.h"

#include <sstream>

namespace aether::editor {

namespace stdfs = std::filesystem;

namespace {

std::vector<std::string> SplitArgs(const std::string& text) {
    std::vector<std::string> args;
    std::istringstream in(text);
    for (std::string a; in >> a;) args.push_back(a);
    return args;
}

} // namespace

PackageRunner::PackageRunner(stdfs::path project_file) : project_file_(std::move(project_file)) {}

PackageRunner::~PackageRunner() {
    Cancel();
    WaitForBuild();
}

stdfs::path PackageRunner::OutputDir() const {
    if (!output_dir.empty()) return output_dir;
    return ProjectPaths::ForFile(project_file_).saved / "Packaged" / cook::ConfigurationName(configuration);
}

void PackageRunner::AddLog(bool warning, std::string text) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_.push_back({warning, std::move(text)});
}

bool PackageRunner::StartCook() { return Start(false); }
bool PackageRunner::StartPackage() { return Start(true); }

bool PackageRunner::Start(bool package) {
    if (busy_.load()) return false;
    pak::PakKey key;
    if (encrypt && !pak::PakKey::FromHex(encryption_key, key)) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_ = {{true, "The encryption key must be 64 hex digits"}};
        stage_ = "Failed: the encryption key must be 64 hex digits";
        result_ = Result::Failed;
        return false;
    }
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
    cook_options.strict_validation = strict_validation;
    cook_options.cancel = &cancel_;
    if (encrypt) cook_options.encryption_key = key;
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

void PackageRunner::Cancel() { cancel_ = true; }

void PackageRunner::WaitForBuild() {
    if (worker_.joinable()) worker_.join();
}

f32 PackageRunner::Progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

std::string PackageRunner::Stage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stage_;
}

std::vector<PackageRunner::LogLine> PackageRunner::Log() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_;
}

PackageRunner::Result PackageRunner::LastResult() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

stdfs::path PackageRunner::StagedGame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return staged_game_;
}

bool PackageRunner::Launch(std::string* error) {
    const stdfs::path game = StagedGame();
    std::string why;
    bool ok = false;
    if (game.empty()) {
        why = "Package the game first";
    } else if (game_.Running()) {
        why = "The game is already running";
    } else {
        std::vector<std::string> args = SplitArgs(launch_args);
        if (encrypt) {
            args.push_back("--key");
            args.push_back(encryption_key);
        }
        ok = game_.Start(game, args, game.parent_path(), &why);
    }
    launch_error_ = ok ? std::string() : why;
    if (!ok && error) *error = why;
    if (ok) AddLog(false, "Launched " + game.filename().string());
    return ok;
}

} // namespace aether::editor
