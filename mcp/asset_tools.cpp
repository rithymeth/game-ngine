#include "asset_tools.h"

#include "project_host.h"
#include "editor_tools.h"
#include "core/commands.h"

#include "aether/assets/asset_database.h"
#include "aether/assets/content_browser.h"
#include "aether/assets/derived_data_cache.h"
#include "aether/assets/importer.h"
#include "aether/cook/cooker.h"
#include "aether/project/project.h"
#include "aether/platform/filesystem.h"
#if AETHER_MCP_TEMPLATES
#include "aether/templates/templates.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace aether::mcp {

namespace {

using namespace detail;

struct ContentFileState {
    bool exists = false;
    std::vector<u8> bytes;
};

ContentFileState CaptureContentFile(const fs::path& path) {
    ContentFileState state;
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory) return state;
    if (ec) throw ToolError("Could not inspect " + path.string() + ": " + ec.message());
    if (!fs::exists(status)) return state;
    if (!fs::is_regular_file(status)) throw ToolError("Not a regular file: " + path.string());
    const auto bytes = fs::file_size(path, ec);
    if (ec) throw ToolError("Could not inspect " + path.string() + ": " + ec.message());
    if (bytes > kMaxTextBytes) throw ToolError("Existing file is larger than 2 MB and cannot be captured for undo: " + path.string());
    state.exists = true;
    if (!aether::fs::ReadFileBytes(path.string(), state.bytes)) throw ToolError("Could not read " + path.string());
    return state;
}

void RestoreContentFile(const fs::path& path, const ContentFileState& state) {
    if (state.exists) {
        const void* data = state.bytes.empty() ? "" : state.bytes.data();
        if (!aether::fs::WriteFileAtomic(path.string(), data, state.bytes.size())) throw ToolError("Could not restore " + path.string());
        return;
    }
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) throw ToolError("Could not remove " + path.string() + ": " + ec.message());
}

class ContentWriteCommand final : public editor::ICommand {
public:
    ContentWriteCommand(std::shared_ptr<AssetHost> host, fs::path project_file, fs::path file, std::string text,
                        ContentFileState before_source, ContentFileState before_meta)
        : host_(std::move(host)), project_file_(std::move(project_file)), file_(std::move(file)),
          before_source_(std::move(before_source)), before_meta_(std::move(before_meta)) {
        after_source_.exists = true;
        after_source_.bytes.assign(text.begin(), text.end());
    }

    void Do(editor::CommandContext&) override {
        RequireSameProject();
        if (!executed_) {
            try {
                WriteSource();
                scan_ = host_->project->database->Scan();
                after_meta_ = CaptureContentFile(MetaFile());
                executed_ = true;
            } catch (...) {
                RestoreContentFile(file_, before_source_);
                RestoreContentFile(MetaFile(), before_meta_);
                host_->project->database->Scan();
                throw;
            }
            return;
        }
        Apply(after_source_, after_meta_, before_source_, before_meta_);
    }

    void Undo(editor::CommandContext&) override {
        RequireSameProject();
        Apply(before_source_, before_meta_, after_source_, after_meta_);
    }

    std::string Label() const override { return "Write Content File"; }
    usize MemoryBytes() const override {
        usize bytes = sizeof(*this) + before_source_.bytes.size() + before_meta_.bytes.size() + after_source_.bytes.size() + after_meta_.bytes.size();
        for (const std::string& warning : scan_.warnings) bytes += warning.size();
        return bytes;
    }
    const assets::ScanResult& Scan() const { return scan_; }

private:
    fs::path MetaFile() const { return fs::path(file_.string() + assets::AssetMeta::kExtension); }
    void RequireSameProject() const {
        if (!host_->project || host_->project->paths.file.lexically_normal() != project_file_.lexically_normal()) {
            throw ToolError("Open the project that owns this content file before undoing or redoing this action");
        }
    }
    void WriteSource() const {
        const void* data = after_source_.bytes.empty() ? "" : after_source_.bytes.data();
        if (!aether::fs::WriteFileAtomic(file_.string(), data, after_source_.bytes.size())) throw ToolError("Could not write " + file_.string());
    }
    void Apply(const ContentFileState& source, const ContentFileState& meta, const ContentFileState& rollback_source,
               const ContentFileState& rollback_meta) {
        try {
            RestoreContentFile(file_, source);
            RestoreContentFile(MetaFile(), meta);
            scan_ = host_->project->database->Scan();
        } catch (...) {
            RestoreContentFile(file_, rollback_source);
            RestoreContentFile(MetaFile(), rollback_meta);
            host_->project->database->Scan();
            throw;
        }
    }

    std::shared_ptr<AssetHost> host_;
    fs::path project_file_;
    fs::path file_;
    ContentFileState before_source_;
    ContentFileState before_meta_;
    ContentFileState after_source_;
    ContentFileState after_meta_;
    assets::ScanResult scan_;
    bool executed_ = false;
};

class ImportSettingsCommand final : public editor::ICommand {
public:
    ImportSettingsCommand(std::shared_ptr<AssetHost> host, fs::path project_file, AssetGuid guid, fs::path meta_file,
                          std::vector<u8> before, assets::AssetMeta after, bool import_now)
        : host_(std::move(host)), project_file_(std::move(project_file)), guid_(guid), meta_file_(std::move(meta_file)),
          before_(std::move(before)), after_(std::move(after)), import_now_(import_now) {}

    void Do(editor::CommandContext&) override {
        RequireSameProject();
        if (!executed_) {
            try {
                SaveAfter();
                host_->project->database->Scan();
                ImportIfRequested();
                after_bytes_ = ReadMeta();
                executed_ = true;
                after_ = assets::AssetMeta{};
            } catch (...) {
                Restore(before_);
                host_->project->database->Scan();
                throw;
            }
            return;
        }
        Apply(after_bytes_, before_);
        after_bytes_ = ReadMeta();
    }

    void Undo(editor::CommandContext&) override {
        RequireSameProject();
        Apply(before_, after_bytes_);
    }

    std::string Label() const override { return "Change Import Settings"; }
    usize MemoryBytes() const override {
        usize bytes = sizeof(*this) + before_.size() + after_bytes_.size() + import_error_.size();
        for (const std::string& warning : warnings_) bytes += warning.size();
        return bytes;
    }
    bool Imported() const { return imported_; }
    bool FromCache() const { return from_cache_; }
    const std::vector<std::string>& Warnings() const { return warnings_; }
    const std::string& ImportError() const { return import_error_; }

private:
    void RequireSameProject() const {
        if (!host_->project || host_->project->paths.file.lexically_normal() != project_file_.lexically_normal()) {
            throw ToolError("Open the project that owns this asset before undoing or redoing this action");
        }
    }
    std::vector<u8> ReadMeta() const {
        std::vector<u8> bytes;
        if (!aether::fs::ReadFileBytes(meta_file_.string(), bytes)) throw ToolError("Could not read " + meta_file_.string());
        return bytes;
    }
    void Restore(const std::vector<u8>& bytes) const {
        const void* data = bytes.empty() ? "" : bytes.data();
        if (!aether::fs::WriteFileAtomic(meta_file_.string(), data, bytes.size())) throw ToolError("Could not restore " + meta_file_.string());
    }
    void SaveAfter() const {
        std::string error;
        if (!assets::SaveAssetMeta(meta_file_, after_, &error)) throw ToolError(error);
    }
    void ImportIfRequested() {
        if (!import_now_) return;
        const assets::ImportOutput result = assets::ImportAsset(*host_->project->database, guid_, host_->project->importers, *host_->project->cache);
        imported_ = result.ok;
        from_cache_ = result.from_cache;
        warnings_ = result.warnings;
        import_error_ = result.error;
    }
    void Apply(const std::vector<u8>& target, const std::vector<u8>& rollback) {
        try {
            Restore(target);
            host_->project->database->Scan();
            ImportIfRequested();
            if (import_now_ && !target.empty()) Restore(target);
            host_->project->database->Scan();
        } catch (...) {
            Restore(rollback);
            host_->project->database->Scan();
            throw;
        }
    }

    std::shared_ptr<AssetHost> host_;
    fs::path project_file_;
    AssetGuid guid_;
    fs::path meta_file_;
    std::vector<u8> before_;
    assets::AssetMeta after_;
    std::vector<u8> after_bytes_;
    bool import_now_ = true;
    bool executed_ = false;
    bool imported_ = false;
    bool from_cache_ = false;
    std::vector<std::string> warnings_;
    std::string import_error_;
};

class CreateContentFolderCommand final : public editor::ICommand {
public:
    CreateContentFolderCommand(std::shared_ptr<AssetHost> host, fs::path project_file, std::string parent, std::string name)
        : host_(std::move(host)), project_file_(std::move(project_file)), parent_(std::move(parent)), name_(std::move(name)) {}

    void Do(editor::CommandContext&) override {
        RequireSameProject();
        std::string error;
        if (!assets::CreateContentFolder(*host_->project->database, parent_, name_, &error)) throw ToolError(error);
    }

    void Undo(editor::CommandContext&) override {
        RequireSameProject();
        std::error_code ec;
        const fs::path folder = host_->project->paths.content / parent_ / name_;
        const bool removed = fs::remove(folder, ec);
        if (ec) throw ToolError("Could not remove " + folder.string() + ": " + ec.message());
        if (!removed) throw ToolError("Could not undo folder creation because the folder is missing or contains files or subfolders: " + folder.string());
    }

    std::string Label() const override { return "Create Content Folder"; }

private:
    void RequireSameProject() const {
        if (!host_->project || host_->project->paths.file.lexically_normal() != project_file_.lexically_normal()) {
            throw ToolError("Open the project that owns this folder before undoing or redoing this action");
        }
    }

    std::shared_ptr<AssetHost> host_;
    fs::path project_file_;
    std::string parent_;
    std::string name_;
};

class ProjectSettingsCommand final : public editor::ICommand {
public:
    ProjectSettingsCommand(std::shared_ptr<AssetHost> host, fs::path project_file, ProjectSettings before, ProjectSettings after)
        : host_(std::move(host)), project_file_(std::move(project_file)), before_(std::move(before)), after_(std::move(after)) {}

    void Do(editor::CommandContext&) override { Apply(after_); }
    void Undo(editor::CommandContext&) override { Apply(before_); }
    std::string Label() const override { return "Change Project Settings"; }
    usize MemoryBytes() const override { return sizeof(*this) + SettingsBytes(before_) + SettingsBytes(after_); }

private:
    static usize ListBytes(const std::vector<std::string>& values) {
        usize bytes = 0;
        for (const std::string& value : values) bytes += value.size();
        return bytes;
    }
    static usize SettingsBytes(const ProjectSettings& settings) {
        usize bytes = settings.name.size() + settings.engine_version.size() + settings.startup_scene.size() + settings.window_title.size() +
                      settings.default_quality.size() + ListBytes(settings.always_cook) + ListBytes(settings.plugins) + ListBytes(settings.layers);
        bytes += settings.collision_matrix.size() * sizeof(u32);
        for (const QualityPreset& preset : settings.quality_presets) bytes += preset.name.size();
        return bytes;
    }
    void Apply(const ProjectSettings& settings) {
        if (!host_->project || host_->project->paths.file.lexically_normal() != project_file_.lexically_normal()) {
            throw ToolError("Open the project that owns these settings before undoing or redoing this action");
        }
        std::string error;
        if (!SaveProject(project_file_, settings, &error)) throw ToolError(error);
        host_->project->settings = settings;
        host_->project->settings.engine_version = kEngineVersion;
    }

    std::shared_ptr<AssetHost> host_;
    fs::path project_file_;
    ProjectSettings before_;
    ProjectSettings after_;
};

class MoveContentCommand final : public editor::ICommand {
public:
    MoveContentCommand(std::shared_ptr<AssetHost> host, fs::path project_file, std::string from, std::string to)
        : host_(std::move(host)), project_file_(std::move(project_file)), from_(std::move(from)), to_(std::move(to)) {}

    void Do(editor::CommandContext&) override { result_ = Move(from_, to_); }
    void Undo(editor::CommandContext&) override {
        const assets::MoveResult reversed = Move(to_, from_);
        if (!reversed.ok) throw ToolError(reversed.error);
    }
    std::string Label() const override { return "Move Content Asset"; }
    usize MemoryBytes() const override {
        usize bytes = sizeof(*this) + from_.size() + to_.size() + result_.error.size();
        for (const std::string& path : result_.rewritten) bytes += path.size();
        return bytes;
    }
    const assets::MoveResult& Result() const { return result_; }

private:
    assets::MoveResult Move(const std::string& from, const std::string& to) {
        if (!host_->project || host_->project->paths.file.lexically_normal() != project_file_.lexically_normal()) {
            throw ToolError("Open the project that owns this content before undoing or redoing this action");
        }
        assets::MoveResult result = assets::MoveContent(*host_->project->database, from, to);
        if (!result.ok) throw ToolError(result.error);
        return result;
    }

    std::shared_ptr<AssetHost> host_;
    fs::path project_file_;
    std::string from_;
    std::string to_;
    assets::MoveResult result_;
};

class DeleteAssetCommand final : public editor::ICommand {
public:
    DeleteAssetCommand(std::shared_ptr<AssetHost> host, fs::path project_file, assets::AssetGuid guid, fs::path source,
                       bool force)
        : host_(std::move(host)), project_file_(std::move(project_file)), guid_(guid), source_(std::move(source)),
          meta_(source_.string() + assets::AssetMeta::kExtension), force_(force) {}

    ~DeleteAssetCommand() override {
        if (!backup_dir_.empty() && !preserve_backup_) {
            std::error_code ec;
            fs::remove_all(backup_dir_, ec);
        }
    }

    void Do(editor::CommandContext&) override {
        RequireSameProject();
        if (!backed_up_) {
            PrepareBackup();
            try {
                DeleteCurrent();
                backed_up_ = true;
            } catch (...) {
                try {
                    RestorePartialDelete();
                } catch (...) {
                    host_->project->database->Scan();
                    throw ToolError("Asset deletion failed and its recovery backup was retained at " + backup_dir_.string());
                }
                host_->project->database->Scan();
                std::error_code ec;
                fs::remove_all(backup_dir_, ec);
                backup_dir_.clear();
                throw;
            }
            return;
        }
        DeleteCurrent();
    }

    void Undo(editor::CommandContext&) override {
        RequireSameProject();
        std::error_code ec;
        if (fs::exists(source_, ec) || fs::exists(meta_, ec)) {
            throw ToolError("Could not restore deleted asset because its original path is occupied");
        }
        if (ec) throw ToolError("Could not check asset restore path: " + ec.message());
        fs::create_directories(source_.parent_path(), ec);
        if (ec) throw ToolError("Could not create " + source_.parent_path().string() + ": " + ec.message());
        bool restored_source = false;
        try {
            if (had_source_) {
                fs::copy_file(backup_source_, source_, ec);
                if (ec) throw ToolError("Could not restore " + source_.string() + ": " + ec.message());
                restored_source = true;
            }
            if (had_meta_) {
                fs::copy_file(backup_meta_, meta_, ec);
                if (ec) throw ToolError("Could not restore " + meta_.string() + ": " + ec.message());
            }
            host_->project->database->Scan();
        } catch (...) {
            if (restored_source) {
                std::error_code remove_error;
                fs::remove(source_, remove_error);
            }
            host_->project->database->Scan();
            throw;
        }
    }

    std::string Label() const override { return "Delete Content Asset"; }
    usize MemoryBytes() const override {
        return sizeof(*this) + project_file_.string().size() + source_.string().size() + meta_.string().size() + backup_dir_.string().size();
    }

private:
    void RequireSameProject() const {
        if (!host_->project || host_->project->paths.file.lexically_normal() != project_file_.lexically_normal()) {
            throw ToolError("Open the project that owns this asset before undoing or redoing this action");
        }
    }

    void PrepareBackup() {
        const fs::path root = host_->project->paths.saved / ".AgentUndo";
        std::error_code ec;
        fs::create_directories(root, ec);
        if (ec) throw ToolError("Could not create undo backup folder: " + ec.message());
        const std::string token = assets::ToString(guid_) + "-" + std::to_string(++next_backup_id_);
        backup_dir_ = root / token;
        if (!fs::create_directory(backup_dir_, ec) || ec) throw ToolError("Could not create undo backup folder: " + ec.message());
        backup_source_ = backup_dir_ / "source";
        backup_meta_ = backup_dir_ / "metadata";
        had_source_ = BackupFile(source_, backup_source_);
        had_meta_ = BackupFile(meta_, backup_meta_);
    }

    bool BackupFile(const fs::path& source, const fs::path& backup) {
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(source, ec);
        if (ec == std::errc::no_such_file_or_directory) return false;
        if (ec) throw ToolError("Could not inspect " + source.string() + ": " + ec.message());
        if (!fs::exists(status)) return false;
        if (!fs::is_regular_file(status)) throw ToolError("Cannot create an undo backup for non-file asset data: " + source.string());
        fs::copy_file(source, backup, ec);
        if (ec) throw ToolError("Could not back up " + source.string() + ": " + ec.message());
        return true;
    }

    void DeleteCurrent() {
        std::string error;
        if (!host_->project->database->Delete(guid_, force_, &error)) throw ToolError(error);
        std::error_code ec;
        if (fs::exists(source_, ec) || fs::exists(meta_, ec) || ec) {
            RestorePartialDelete();
            host_->project->database->Scan();
            throw ToolError("The asset database could not remove all files for " + source_.string());
        }
        host_->project->database->Scan();
    }

    void RestorePartialDelete() {
        try {
            const auto restore = [](const fs::path& destination, const fs::path& backup, bool existed) {
                if (!existed) return;
                std::error_code ec;
                if (fs::exists(destination, ec)) return;
                if (ec) throw ToolError("Could not inspect " + destination.string() + ": " + ec.message());
                fs::copy_file(backup, destination, ec);
                if (ec) throw ToolError("Could not restore " + destination.string() + ": " + ec.message());
            };
            restore(source_, backup_source_, had_source_);
            restore(meta_, backup_meta_, had_meta_);
        } catch (...) {
            preserve_backup_ = true;
            throw;
        }
    }

    inline static std::atomic<std::uint64_t> next_backup_id_{0};
    std::shared_ptr<AssetHost> host_;
    fs::path project_file_;
    assets::AssetGuid guid_;
    fs::path source_;
    fs::path meta_;
    fs::path backup_dir_;
    fs::path backup_source_;
    fs::path backup_meta_;
    bool force_ = false;
    bool had_source_ = false;
    bool had_meta_ = false;
    bool backed_up_ = false;
    bool preserve_backup_ = false;
};

class AddFilesCommand final : public editor::ICommand {
public:
    struct File {
        fs::path input;
        fs::path destination;
        fs::path meta;
        fs::path staged_source;
        fs::path before_source;
        fs::path before_meta;
        fs::path after_meta;
        bool had_before_source = false;
        bool had_before_meta = false;
        bool has_after_meta = false;
    };

    AddFilesCommand(std::shared_ptr<AssetHost> host, fs::path project_file, fs::path backup_root,
                    std::vector<File> files, bool import_now)
        : host_(std::move(host)), project_file_(std::move(project_file)), backup_root_(std::move(backup_root)),
          files_(std::move(files)), import_now_(import_now) {}

    ~AddFilesCommand() override {
        if (!backup_dir_.empty() && !preserve_backup_) {
            std::error_code ec;
            fs::remove_all(backup_dir_, ec);
        }
    }

    void Do(editor::CommandContext&) override {
        RequireSameProject();
        if (!prepared_) Prepare();
        if (applied_) {
            VerifyBeforeState();
            EnsureCreatedDirectories();
            CopyNewFiles();
            RestoreAfterMetadata();
            scan_ = host_->project->database->Scan();
            return;
        }
        try {
            VerifyBeforeState();
            CaptureCreatedDirectories();
            EnsureCreatedDirectories();
            CopyNewFiles();
            scan_ = host_->project->database->Scan();
            ImportAddedAssets();
            CaptureAfterMetadata();
            applied_ = true;
        } catch (...) {
            try {
                RestoreBeforeState();
            } catch (...) {
                preserve_backup_ = true;
            }
            host_->project->database->Scan();
            throw;
        }
    }

    void Undo(editor::CommandContext&) override {
        RequireSameProject();
        VerifyAfterState();
        RestoreBeforeState();
        RemoveCreatedDirectories();
        scan_ = host_->project->database->Scan();
    }

    std::string Label() const override { return "Add Content Files"; }
    usize MemoryBytes() const override {
        usize bytes = sizeof(*this) + project_file_.string().size() + backup_dir_.string().size();
        for (const File& file : files_) bytes += file.destination.string().size() + file.meta.string().size();
        for (const std::string& error : imports_.errors) bytes += error.size();
        return bytes;
    }
    const std::vector<File>& Files() const { return files_; }
    const assets::ScanResult& Scan() const { return scan_; }
    const assets::ImportAllResult& Imports() const { return imports_; }
    bool Imported() const { return import_now_; }

private:
    void RequireSameProject() const {
        if (!host_->project || host_->project->paths.file.lexically_normal() != project_file_.lexically_normal()) {
            throw ToolError("Open the project that owns these files before undoing or redoing this action");
        }
    }

    static bool Exists(const fs::path& path) {
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(path, ec);
        if (ec == std::errc::no_such_file_or_directory) return false;
        if (ec) throw ToolError("Could not inspect " + path.string() + ": " + ec.message());
        return fs::exists(status);
    }

    static bool CopySnapshot(const fs::path& source, const fs::path& snapshot) {
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(source, ec);
        if (ec == std::errc::no_such_file_or_directory) return false;
        if (ec) throw ToolError("Could not inspect " + source.string() + ": " + ec.message());
        if (!fs::exists(status)) return false;
        if (!fs::is_regular_file(status)) throw ToolError("Cannot undo a non-file asset path: " + source.string());
        fs::copy_file(source, snapshot, ec);
        if (ec) throw ToolError("Could not back up " + source.string() + ": " + ec.message());
        return true;
    }

    static bool MatchesSnapshot(const fs::path& path, bool should_exist, const fs::path& snapshot) {
        const bool exists = Exists(path);
        if (!should_exist) return !exists;
        if (!exists) return false;
        std::error_code ec;
        const std::uintmax_t actual_size = fs::file_size(path, ec);
        if (ec) return false;
        const std::uintmax_t snapshot_size = fs::file_size(snapshot, ec);
        if (ec || actual_size != snapshot_size) return false;
        std::ifstream actual(path, std::ios::binary), expected(snapshot, std::ios::binary);
        if (!actual || !expected) return false;
        char a[64 * 1024], b[64 * 1024];
        while (actual && expected) {
            actual.read(a, sizeof(a));
            expected.read(b, sizeof(b));
            if (actual.gcount() != expected.gcount() || !std::equal(a, a + actual.gcount(), b)) return false;
        }
        return actual.eof() && expected.eof();
    }

    void Prepare() {
        std::error_code ec;
        fs::create_directories(backup_root_, ec);
        if (ec) throw ToolError("Could not create add-files undo backup folder: " + ec.message());
        backup_dir_ = backup_root_ / ("add-files-" + std::to_string(++next_backup_id_));
        if (!fs::create_directory(backup_dir_, ec) || ec) throw ToolError("Could not create add-files undo backup folder: " + ec.message());
        for (usize i = 0; i < files_.size(); ++i) {
            File& file = files_[i];
            const fs::path index = std::to_string(i);
            file.staged_source = backup_dir_ / (index.string() + ".source");
            file.before_source = backup_dir_ / (index.string() + ".before");
            file.before_meta = backup_dir_ / (index.string() + ".before-meta");
            file.after_meta = backup_dir_ / (index.string() + ".after-meta");
            file.had_before_source = CopySnapshot(file.destination, file.before_source);
            file.had_before_meta = CopySnapshot(file.meta, file.before_meta);
            if (!CopySnapshot(file.input, file.staged_source)) throw ToolError("Not a file: " + file.input.string());
        }
        prepared_ = true;
    }

    void VerifyBeforeState() const {
        for (const File& file : files_) {
            if (!MatchesSnapshot(file.destination, file.had_before_source, file.before_source) ||
                !MatchesSnapshot(file.meta, file.had_before_meta, file.before_meta)) {
                throw ToolError("Cannot apply add-files action because a destination changed: " + file.destination.string());
            }
        }
    }

    void VerifyAfterState() const {
        for (const File& file : files_) {
            if (!MatchesSnapshot(file.destination, true, file.staged_source) ||
                !MatchesSnapshot(file.meta, file.has_after_meta, file.after_meta)) {
                throw ToolError("Cannot undo add-files action because a destination changed: " + file.destination.string());
            }
        }
    }

    void CopyNewFiles() {
        for (const File& file : files_) {
            std::error_code ec;
            fs::create_directories(file.destination.parent_path(), ec);
            if (ec) throw ToolError("Could not create " + file.destination.parent_path().string() + ": " + ec.message());
            fs::copy_file(file.staged_source, file.destination, fs::copy_options::overwrite_existing, ec);
            if (ec) throw ToolError("Could not copy staged asset to " + file.destination.string() + ": " + ec.message());
        }
    }

    void RestoreAfterMetadata() {
        for (const File& file : files_) {
            std::error_code ec;
            if (file.has_after_meta) {
                fs::copy_file(file.after_meta, file.meta, fs::copy_options::overwrite_existing, ec);
                if (ec) throw ToolError("Could not restore " + file.meta.string() + ": " + ec.message());
            } else {
                fs::remove(file.meta, ec);
                if (ec == std::errc::no_such_file_or_directory) ec.clear();
                if (ec) throw ToolError("Could not remove " + file.meta.string() + ": " + ec.message());
            }
        }
    }

    void RestoreBeforeState() {
        for (const File& file : files_) {
            RestoreOne(file.destination, file.before_source, file.had_before_source);
            RestoreOne(file.meta, file.before_meta, file.had_before_meta);
        }
    }

    void CaptureCreatedDirectories() {
        const fs::path content = host_->project->paths.content.lexically_normal();
        std::set<fs::path> missing;
        for (const File& file : files_) {
            fs::path dir = file.destination.parent_path().lexically_normal();
            while (dir != content) {
                const fs::path relative = dir.lexically_relative(content);
                if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
                    throw ToolError("Destination is outside Content/: " + dir.string());
                }
                std::error_code ec;
                const fs::file_status status = fs::symlink_status(dir, ec);
                if (!ec && fs::exists(status)) {
                    if (fs::is_symlink(status) || !fs::is_directory(status)) throw ToolError("Destination parent is not a regular folder: " + dir.string());
                    break;
                }
                if (ec == std::errc::no_such_file_or_directory) ec.clear();
                if (ec) throw ToolError("Could not inspect " + dir.string() + ": " + ec.message());
                missing.insert(dir);
                dir = dir.parent_path();
            }
        }
        created_directories_.assign(missing.begin(), missing.end());
        std::sort(created_directories_.begin(), created_directories_.end(), [](const fs::path& a, const fs::path& b) {
            return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end());
        });
    }

    void EnsureCreatedDirectories() {
        for (const fs::path& dir : created_directories_) {
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (ec) throw ToolError("Could not create " + dir.string() + ": " + ec.message());
        }
    }

    void RemoveCreatedDirectories() {
        for (const fs::path& dir : created_directories_) {
            std::error_code ec;
            fs::remove(dir, ec); // Keep any directory that now contains unrelated files.
        }
    }

    static void RestoreOne(const fs::path& destination, const fs::path& snapshot, bool existed) {
        std::error_code ec;
        if (existed) {
            fs::copy_file(snapshot, destination, fs::copy_options::overwrite_existing, ec);
            if (ec) throw ToolError("Could not restore " + destination.string() + ": " + ec.message());
        } else {
            fs::remove(destination, ec);
            if (ec == std::errc::no_such_file_or_directory) ec.clear();
            if (ec) throw ToolError("Could not remove " + destination.string() + ": " + ec.message());
        }
    }

    void ImportAddedAssets() {
        if (!import_now_) return;
        imports_ = {};
        for (const File& file : files_) {
            const std::string path = fs::relative(file.destination, host_->project->paths.content).generic_string();
            const AssetRecord* record = host_->project->database->FindByPath(path);
            if (record == nullptr) {
                ++imports_.skipped;
                continue;
            }
            if (host_->project->importers.Find(record->importer) == nullptr) {
                ++imports_.skipped;
                continue;
            }
            const assets::ImportOutput result = assets::ImportAsset(*host_->project->database, record->guid,
                                                                    host_->project->importers, *host_->project->cache);
            if (!result.ok) {
                ++imports_.failed;
                imports_.errors.push_back(path + ": " + result.error);
            } else if (result.from_cache) {
                ++imports_.from_cache;
            } else {
                ++imports_.imported;
            }
        }
    }

    void CaptureAfterMetadata() {
        for (File& file : files_) file.has_after_meta = CopySnapshot(file.meta, file.after_meta);
    }

    inline static std::atomic<std::uint64_t> next_backup_id_{0};
    std::shared_ptr<AssetHost> host_;
    fs::path project_file_;
    fs::path backup_root_;
    fs::path backup_dir_;
    std::vector<File> files_;
    std::vector<fs::path> created_directories_;
    bool import_now_ = true;
    bool prepared_ = false;
    bool applied_ = false;
    bool preserve_backup_ = false;
    assets::ScanResult scan_;
    assets::ImportAllResult imports_;
};

class AssetImportCommand final : public editor::ICommand {
public:
    struct SubAsset {
        std::string key;
        std::string importer;
        AssetGuid guid;
    };
    struct SingleResult {
        bool ok = false;
        bool from_cache = false;
        usize bytes = 0;
        std::vector<std::string> warnings;
        std::vector<SubAsset> sub_assets;
        std::string error;
    };

    AssetImportCommand(std::shared_ptr<AssetHost> host, fs::path project_file, std::optional<AssetGuid> guid)
        : host_(std::move(host)), project_file_(std::move(project_file)), guid_(guid) {}

    void Do(editor::CommandContext&) override {
        RequireSameProject();
        if (executed_) {
            Apply(after_, before_);
            return;
        }
        before_ = CaptureMetadata();
        try {
            if (guid_) {
                const assets::ImportOutput output = assets::ImportAsset(*host_->project->database, *guid_,
                                                                          host_->project->importers, *host_->project->cache);
                single_.ok = output.ok;
                single_.from_cache = output.from_cache;
                single_.bytes = output.data.size();
                single_.warnings = output.warnings;
                single_.error = output.error;
                for (const assets::SubAssetOutput& sub : output.sub_assets) {
                    single_.sub_assets.push_back({sub.key, sub.importer, sub.guid});
                }
            } else {
                imports_ = assets::ImportAll(*host_->project->database, host_->project->importers, *host_->project->cache);
            }
            after_ = CaptureCurrentMetadata(before_);
            scan_ = host_->project->database->Scan();
            executed_ = true;
        } catch (...) {
            RestoreMetadata(before_);
            host_->project->database->Scan();
            throw;
        }
    }

    void Undo(editor::CommandContext&) override {
        RequireSameProject();
        Apply(before_, after_);
    }

    std::string Label() const override { return guid_ ? "Import Asset" : "Import Pending Assets"; }
    usize MemoryBytes() const override {
        usize bytes = sizeof(*this) + project_file_.string().size() + single_.error.size();
        auto count = [&](const auto& files) {
            for (const auto& file : files) bytes += file.state.bytes.size();
        };
        count(before_);
        count(after_);
        for (const std::string& warning : single_.warnings) bytes += warning.size();
        for (const SubAsset& sub : single_.sub_assets) bytes += sub.key.size() + sub.importer.size();
        for (const std::string& error : imports_.errors) bytes += error.size();
        return bytes;
    }
    const SingleResult& Single() const { return single_; }
    const assets::ImportAllResult& All() const { return imports_; }

private:
    struct MetadataFile {
        fs::path path;
        ContentFileState state;
    };

    std::vector<MetadataFile> CaptureMetadata() const {
        std::vector<MetadataFile> files;
        std::set<fs::path> seen;
        auto capture = [&](const AssetRecord* record) {
            if (record == nullptr || record->IsSubAsset()) return;
            const fs::path path = host_->project->database->MetaPath(record->guid);
            if (seen.insert(path).second) files.push_back({path, CaptureContentFile(path)});
        };
        if (guid_) {
            const AssetRecord* record = host_->project->database->Find(*guid_);
            if (record != nullptr && record->IsSubAsset()) record = host_->project->database->Find(record->parent);
            capture(record);
        } else {
            for (const AssetRecord* record : host_->project->database->All()) {
                if (record->needs_import) capture(record);
            }
        }
        return files;
    }

    static void RestoreMetadata(const std::vector<MetadataFile>& files) {
        for (const MetadataFile& file : files) RestoreContentFile(file.path, file.state);
    }

    static std::vector<MetadataFile> CaptureCurrentMetadata(const std::vector<MetadataFile>& files) {
        std::vector<MetadataFile> current;
        current.reserve(files.size());
        for (const MetadataFile& file : files) current.push_back({file.path, CaptureContentFile(file.path)});
        return current;
    }

    void RequireSameProject() const {
        if (!host_->project || host_->project->paths.file.lexically_normal() != project_file_.lexically_normal()) {
            throw ToolError("Open the project that owns these assets before undoing or redoing this import");
        }
    }

    void Apply(const std::vector<MetadataFile>& target, const std::vector<MetadataFile>& rollback) {
        try {
            RestoreMetadata(target);
            scan_ = host_->project->database->Scan();
        } catch (...) {
            RestoreMetadata(rollback);
            host_->project->database->Scan();
            throw;
        }
    }

    std::shared_ptr<AssetHost> host_;
    fs::path project_file_;
    std::optional<AssetGuid> guid_;
    std::vector<MetadataFile> before_;
    std::vector<MetadataFile> after_;
    assets::ScanResult scan_;
    SingleResult single_;
    assets::ImportAllResult imports_;
    bool executed_ = false;
};

Json ProjectJson(OpenProject& p) {
    usize sources = 0;
    for (const AssetRecord* r : p.database->All()) {
        if (!r->IsSubAsset()) ++sources;
    }
    return {{"name", p.settings.name},
            {"project_file", p.paths.file.string()},
            {"content", p.paths.content.string()},
            {"engine_version", p.settings.engine_version},
            {"startup_scene", p.settings.startup_scene},
            {"always_cook", p.settings.always_cook},
            {"plugins", p.settings.plugins},
            {"assets", sources}};
}

Json OpenProjectFile(AssetHost& host, const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) throw ToolError("No such project file: " + file.string());
    auto project = std::make_unique<OpenProject>();
    std::string error;
    std::vector<std::string> warnings;
    if (!LoadProject(file, project->settings, &error, &warnings)) throw ToolError(error);
    project->paths = ProjectPaths::ForFile(fs::absolute(file, ec));
    if (!EnsureProjectFolders(project->paths, &error)) throw ToolError(error);
    project->database = std::make_unique<AssetDatabase>(project->paths.content);
    const assets::ScanResult scan = project->database->Scan();
    project->cache = std::make_unique<assets::DerivedDataCache>(project->paths.intermediate / "DerivedDataCache");
    host.project = std::move(project);
    Json out = ProjectJson(*host.project);
    out["scan"] = ScanJson(scan);
    out["warnings"] = warnings;
    return out;
}

// A file inside Content/ for reading or writing text.
fs::path ContentFile(OpenProject& p, const std::string& relative) {
    const std::string rel = CleanRelative(relative);
    if (rel.empty()) throw ToolError("\"path\" must name a file under Content/");
    return p.paths.content / rel;
}

std::string KindOfCookFailure(const cook::CookReport& r) { return r.error.empty() ? "The cook failed" : r.error; }

} // namespace

void RegisterAssetTools(McpServer& server, std::shared_ptr<detail::AssetHost> host, EditorSession& session) {

    server.AddTool(
        {"project_open",
         "Open a project (.aproject) for the asset and cook tools: reads its settings and scans Content/ (new source files get an "
         ".ameta with a GUID). Replaces any open project.",
         Schema({{"project_file", {{"type", "string"}, {"description", "Path to the .aproject file"}}}}, {"project_file"}),
         [host](const Json& args) -> Json { return OpenProjectFile(*host, RequireString(args, "project_file")); }});

    server.AddTool(
        {"project_templates",
         "The project templates project_create can start from (blank, first_person, third_person, top_down, vehicle, platformer_2d): id, name, genre, description, what each "
         "contains, and whether it is available in this build.",
         Schema(Json::object()),
         [](const Json&) -> Json {
             Json out = Json::array();
#if AETHER_MCP_TEMPLATES
             for (const templates::ProjectTemplate& t : templates::ProjectTemplates()) {
                 Json j = {{"id", t.id}, {"name", t.name}, {"genre", t.genre}, {"description", t.description}, {"features", t.features}, {"available", t.available}};
                 if (!t.available) j["unavailable_reason"] = t.unavailable_reason;
                 out.push_back(j);
             }
#endif
             return {{"count", out.size()}, {"templates", out}};
         }});

    server.AddTool(
        {"project_create",
         "Create a new project in `<parent_dir>/<name>/` (the standard folders and `<name>.aproject`) and open it for the asset and cook tools. With `template` (see project_templates) "
         "the project starts from that template: a startup scene, a controller script, input bindings and a Blueprint. Fails if the folder exists and isn't empty, or the name isn't a "
         "valid folder name.",
         Schema({{"parent_dir", {{"type", "string"}, {"description", "Folder to make the project folder in (made if missing)"}}},
                 {"name", {{"type", "string"}}},
                 {"template", {{"type", "string"}, {"description", "A template id from project_templates; omit for an empty project"}}}},
                {"parent_dir", "name"}),
         [host](const Json& args) -> Json {
             const fs::path parent = RequireString(args, "parent_dir");
             const std::string name = RequireString(args, "name");
             const std::string template_id = OptString(args, "template");
             std::error_code ec;
             fs::create_directories(parent, ec);
             ProjectPaths paths;
             std::string error;
             if (template_id.empty()) {
                 if (!CreateProject(parent, name, &paths, &error)) throw ToolError(error);
             } else {
#if AETHER_MCP_TEMPLATES
                 if (!templates::CreateProjectFromTemplate(parent, name, template_id, &paths, &error)) throw ToolError(error);
#else
                 throw ToolError("This build has no project templates");
#endif
             }
             Json out = OpenProjectFile(*host, paths.file);
             if (!template_id.empty()) out["template"] = template_id;
             return out;
         }});

    server.AddTool({"project_info", "Settings and asset count of the open project.", Schema(Json::object()),
                    [host](const Json&) -> Json {
                        OpenProject& p = host->Require();
                        Json out = ProjectJson(p);
                        out["layers"] = p.settings.layers;
                        out["fixed_timestep_hz"] = p.settings.fixed_timestep_hz;
                        out["window"] = {{"title", p.settings.window_title}, {"width", p.settings.window_width}, {"height", p.settings.window_height}};
                        return out;
                    }});

    server.AddTool(
        {"project_set",
         "Change project settings and save the .aproject as one agent-attributed undoable action: startup_scene, always_cook (asset paths or folders ending in '/'), plugins.",
         Schema({{"startup_scene", {{"type", "string"}}},
                 {"always_cook", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                 {"plugins", {{"type", "array"}, {"items", {{"type", "string"}}}}}}),
         [host, &session](const Json& args) -> Json {
             OpenProject& p = host->Require();
             if (session.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing project settings");
             const ProjectSettings before = p.settings;
             ProjectSettings next = p.settings;
             if (args.contains("startup_scene")) next.startup_scene = OptString(args, "startup_scene");
             if (args.contains("always_cook")) next.always_cook = OptStrings(args, "always_cook");
             if (args.contains("plugins")) next.plugins = OptStrings(args, "plugins");
             auto command = std::make_unique<ProjectSettingsCommand>(host, p.paths.file, before, std::move(next));
             command->SetAttribution("agent");
             editor::CommandContext context = session.Context();
             session.stack.Execute(context, std::move(command));
             return ProjectJson(p);
         }});

    server.AddTool(
        {"asset_list",
         "List content: assets (and folders) under a folder, filtered by name and type. Types are importer names such as "
         "Texture, Model, Scene, Prefab.",
         Schema({{"folder", {{"type", "string"}, {"description", "Relative to Content/ (default: the root)"}}},
                 {"search", {{"type", "string"}, {"description", "Case-insensitive part of the name; searches subfolders"}}},
                 {"types", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                 {"recursive", {{"type", "boolean"}}},
                 {"include_sub_assets", {{"type", "boolean"}}},
                 {"limit", {{"type", "integer"}, {"description", "Default 200"}}}}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             assets::ContentQuery q;
             q.folder = CleanRelative(OptString(args, "folder"));
             q.search = OptString(args, "search");
             q.types = OptStrings(args, "types");
             q.recursive = OptBool(args, "recursive", false);
             q.include_sub_assets = OptBool(args, "include_sub_assets", false);
             usize limit = 200;
             if (args.contains("limit") && args["limit"].is_number_integer()) limit = static_cast<usize>(std::max<long long>(1, args["limit"].get<long long>()));
             const std::vector<assets::ContentEntry> entries = assets::ListContent(*p.database, q);
             Json folders = Json::array(), list = Json::array();
             for (const assets::ContentEntry& e : entries) {
                 if (e.is_folder) {
                     folders.push_back(e.path);
                 } else if (e.asset && list.size() < limit) {
                     list.push_back(RecordJson(p, *e.asset));
                 }
             }
             return {{"total", entries.size()}, {"folders", folders}, {"assets", list}};
         }});

    server.AddTool({"asset_info",
                    "Everything about one asset: importer, import settings, dependencies, what references it, sub-assets, import state.",
                    Schema({{"asset", {{"type", "string"}, {"description", "Content path or GUID"}}}}, {"asset"}),
                    [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        return RecordJson(p, FindAsset(p, RequireString(args, "asset")), true);
                    }});

    server.AddTool({"asset_references",
                    "The reference graph of an asset: what it uses (dependencies) or what uses it (referencers), to a depth.",
                    Schema({{"asset", {{"type", "string"}}},
                            {"direction", {{"type", "string"}, {"description", "dependencies (default) or referencers"}}},
                            {"max_depth", {{"type", "integer"}, {"description", "Default 4, max 16"}}}},
                           {"asset"}),
                    [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        const AssetRecord& r = FindAsset(p, RequireString(args, "asset"));
                        const std::string dir = OptString(args, "direction", "dependencies");
                        if (dir != "dependencies" && dir != "referencers") throw ToolError("\"direction\" must be dependencies or referencers");
                        long long depth = 4;
                        if (args.contains("max_depth") && args["max_depth"].is_number_integer()) depth = std::clamp<long long>(args["max_depth"].get<long long>(), 1, 16);
                        Json nodes = Json::array();
                        for (const assets::ReferenceNode& n : assets::CollectReferences(
                                 *p.database, r.guid, dir == "referencers" ? assets::ReferenceDirection::Referencers : assets::ReferenceDirection::Dependencies,
                                 static_cast<u32>(depth))) {
                            nodes.push_back({{"path", n.path}, {"depth", n.depth}, {"repeated", n.repeated}});
                        }
                        return {{"asset", r.path}, {"direction", dir}, {"nodes", nodes}};
                    }});

    server.AddTool({"asset_scan",
                    "Rescan Content/: new files get GUIDs, changed sources are flagged needs_import, missing ones are reported.",
                    Schema(Json::object()), [host](const Json&) -> Json { return ScanJson(host->Require().database->Scan()); }});

    server.AddTool(
        {"asset_add_files",
         "Copy source files (textures, models, audio...) into Content/<folder> as one agent-attributed undoable action, then scan "
         "and import only those files. Undo restores overwritten source files and .ameta sidecars. Files keep their names (a glTF's "
         ".bin and textures should be added together, in the same folder). Existing files are not overwritten unless `overwrite`.",
         Schema({{"sources", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Absolute paths of files to copy in"}}},
                 {"folder", {{"type", "string"}, {"description", "Destination folder under Content/ (created if needed)"}}},
                 {"overwrite", {{"type", "boolean"}}},
                 {"import", {{"type", "boolean"}, {"description", "Import them now (default true)"}}}},
                {"sources"}),
         [host, &session](const Json& args) -> Json {
             OpenProject& p = host->Require();
             if (session.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before adding content files");
             const std::vector<std::string> sources = OptStrings(args, "sources");
             if (sources.empty()) throw ToolError("\"sources\" must be a non-empty array of file paths");
             const std::string folder = CleanRelative(OptString(args, "folder"));
             for (const fs::path& part : fs::path(folder)) {
                 std::string why;
                 if (!assets::IsValidAssetName(part.string(), &why)) throw ToolError("Bad destination folder name \"" + part.string() + "\": " + why);
             }
             const bool overwrite = OptBool(args, "overwrite", false);
             const fs::path dest_dir = folder.empty() ? p.paths.content : p.paths.content / folder;
             std::vector<AddFilesCommand::File> files;
             std::set<std::string> destinations;
             for (const std::string& src : sources) {
                 const fs::path from = src;
                 std::error_code ec;
                 const fs::file_status status = fs::symlink_status(from, ec);
                 if (ec || !fs::is_regular_file(status)) throw ToolError("Not a file: " + src);
                 std::string why;
                 if (!assets::IsValidAssetName(from.filename().string(), &why)) throw ToolError("Bad file name \"" + from.filename().string() + "\": " + why);
                 const fs::path to = dest_dir / from.filename();
                 const std::string rel = (folder.empty() ? "" : folder + "/") + from.filename().generic_string();
                 if (!destinations.insert(rel).second) throw ToolError("Duplicate destination file: " + rel);
                 if (fs::exists(to, ec) && !overwrite) throw ToolError(to.filename().string() + " already exists in " + (folder.empty() ? "Content/" : folder) + " (pass overwrite)");
                 if (ec) throw ToolError("Could not inspect " + to.string() + ": " + ec.message());
                 AddFilesCommand::File file;
                 file.input = from;
                 file.destination = to;
                 file.meta = fs::path(to.string() + assets::AssetMeta::kExtension);
                 files.push_back(std::move(file));
             }
             auto command = std::make_unique<AddFilesCommand>(host, p.paths.file, p.paths.saved / ".AgentUndo", std::move(files), OptBool(args, "import", true));
             command->SetAttribution("agent");
             AddFilesCommand* command_result = command.get();
             editor::CommandContext context = session.Context();
             session.stack.Execute(context, std::move(command));
             Json copied = Json::array();
             for (const auto& file : command_result->Files()) copied.push_back(fs::relative(file.destination, p.paths.content).generic_string());
             Json out = {{"copied", copied}, {"scan", ScanJson(command_result->Scan())}};
             if (command_result->Imported()) {
                 const assets::ImportAllResult& r = command_result->Imports();
                 out["import"] = {{"imported", r.imported}, {"from_cache", r.from_cache}, {"failed", r.failed}, {"skipped", r.skipped}, {"errors", r.errors}};
             }
             Json records = Json::array();
             for (const Json& c : copied) {
                 if (const AssetRecord* r = p.database->FindByPath(c.get<std::string>())) records.push_back(RecordJson(p, *r));
             }
             out["assets"] = records;
             return out;
         }});

    server.AddTool(
        {"asset_import",
         "Run the importers. With `asset`, (re)import one asset; with `assets`, import a selected array of up to 128 assets as one undoable group; "
         "without either, import everything flagged needs_import. An asset or assets cannot be combined. Import status and sub-asset metadata "
         "are undoable. Results come from the derived-data cache when the input is unchanged.",
         Schema({{"asset", {{"type", "string"}, {"description", "Content path or GUID; omit to import all pending"}}},
                 {"assets", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Selected content paths or GUIDs to import as one undoable group (1-128)"}}}}),
         [host, &session](const Json& args) -> Json {
             OpenProject& p = host->Require();
             if (session.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before importing assets");
             if (args.contains("asset") && args.contains("assets")) throw ToolError("Pass either \"asset\" or \"assets\", not both");
             if (args.contains("assets")) {
                 if (!args["assets"].is_array() || args["assets"].empty() || args["assets"].size() > 128) {
                     throw ToolError("\"assets\" must be a non-empty array of at most 128 asset paths or GUIDs");
                 }
                 std::vector<std::pair<AssetGuid, std::string>> selected;
                 std::set<AssetGuid> seen;
                 for (const Json& value : args["assets"]) {
                     if (!value.is_string()) throw ToolError("Each entry in \"assets\" must be a path or GUID string");
                     const AssetRecord* record = &FindAsset(p, value.get<std::string>());
                     if (record->IsSubAsset()) record = p.database->Find(record->parent);
                     if (record == nullptr || record->IsSubAsset()) throw ToolError("Could not resolve the source asset for " + value.get<std::string>());
                     if (!seen.insert(record->guid).second) throw ToolError("Duplicate asset in \"assets\": " + record->path);
                     selected.emplace_back(record->guid, record->path);
                 }

                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Import Assets " + std::to_string(++next_batch_id);
                 const usize undo_count_before = session.stack.UndoCount();
                 Json results = Json::array();
                 usize imported = 0, from_cache = 0, failed = 0;
                 editor::CommandContext context = session.Context();
                 session.stack.BeginTransaction(transaction_label);
                 try {
                     for (const auto& [guid, path] : selected) {
                         auto command = std::make_unique<AssetImportCommand>(host, p.paths.file, guid);
                         command->SetAttribution("agent");
                         AssetImportCommand* command_result = command.get();
                         session.stack.Execute(context, std::move(command));
                         const AssetImportCommand::SingleResult& out = command_result->Single();
                         Json row = {{"ok", out.ok}, {"asset", path}, {"from_cache", out.from_cache}, {"bytes", out.bytes}, {"warnings", out.warnings}};
                         Json subs = Json::array();
                         for (const AssetImportCommand::SubAsset& sub : out.sub_assets) {
                             subs.push_back({{"key", sub.key}, {"importer", sub.importer}, {"guid", assets::ToString(sub.guid)}});
                         }
                         if (!subs.empty()) row["sub_assets"] = std::move(subs);
                         if (!out.ok) row["error"] = out.error;
                         results.push_back(std::move(row));
                         if (out.ok) ++imported;
                         else ++failed;
                         if (out.from_cache) ++from_cache;
                     }
                     session.stack.EndTransaction();
                 } catch (...) {
                     session.stack.EndTransaction();
                     if (session.stack.UndoCount() != undo_count_before || session.stack.UndoLabel() == transaction_label) {
                         session.stack.Undo(context);
                     }
                     throw;
                 }
                 return {{"imported", imported}, {"from_cache", from_cache}, {"failed", failed}, {"skipped", 0}, {"results", results}};
             }
             std::optional<AssetGuid> guid;
             std::string path;
             if (args.contains("asset")) {
                 const AssetRecord& record = FindAsset(p, RequireString(args, "asset"));
                 guid = record.guid;
                 path = record.path;
             } else {
                 const std::vector<const AssetRecord*> records = p.database->All();
                 const bool has_pending = std::any_of(records.begin(), records.end(), [](const AssetRecord* record) {
                     return record->needs_import && !record->missing;
                 });
                 if (!has_pending) return {{"imported", 0}, {"from_cache", 0}, {"failed", 0}, {"skipped", 0}, {"errors", Json::array()}};
             }
             auto command = std::make_unique<AssetImportCommand>(host, p.paths.file, guid);
             command->SetAttribution("agent");
             AssetImportCommand* command_result = command.get();
             editor::CommandContext context = session.Context();
             session.stack.Execute(context, std::move(command));
             if (!guid) {
                 const assets::ImportAllResult& r = command_result->All();
                 return {{"imported", r.imported}, {"from_cache", r.from_cache}, {"failed", r.failed}, {"skipped", r.skipped}, {"errors", r.errors}};
             }
             const AssetImportCommand::SingleResult& out = command_result->Single();
             Json j = {{"ok", out.ok}, {"asset", path}, {"from_cache", out.from_cache}, {"bytes", out.bytes}, {"warnings", out.warnings}};
             Json subs = Json::array();
             for (const AssetImportCommand::SubAsset& s : out.sub_assets) subs.push_back({{"key", s.key}, {"importer", s.importer}, {"guid", assets::ToString(s.guid)}});
             if (!subs.empty()) j["sub_assets"] = subs;
             if (!out.ok) j["error"] = out.error;
             return j;
         }});

    server.AddTool(
        {"asset_set_import_settings",
         "Merge importer settings into an asset's .ameta (e.g. a texture's {\"cook_format\": \"bc7\", \"srgb\": false}) and mark it "
         "for reimport; with `import` (default true) reimport it now. `labels` replaces its labels.",
         Schema({{"asset", {{"type", "string"}}},
                 {"settings", {{"type", "object"}, {"description", "Merged over the current settings (null removes a key)"}}},
                 {"labels", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                 {"import", {{"type", "boolean"}}}},
                {"asset"}),
         [host, &session](const Json& args) -> Json {
             OpenProject& p = host->Require();
             if (session.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing import settings");
             const AssetRecord& rec = FindAsset(p, RequireString(args, "asset"));
             if (rec.IsSubAsset()) throw ToolError("Sub-assets take their source's settings: change " + PathOf(p, rec.parent));
             const AssetGuid guid = rec.guid;
             const fs::path meta_file = p.database->MetaPath(guid);
             std::vector<u8> before;
             if (!aether::fs::ReadFileBytes(meta_file.string(), before)) throw ToolError("Couldn't read " + meta_file.string());
             assets::AssetMeta meta;
             std::string error;
             if (!assets::LoadAssetMetaFromMemory(before, meta, &error)) throw ToolError(error);
             if (args.contains("settings")) {
                 if (!args["settings"].is_object()) throw ToolError("\"settings\" must be an object");
                 meta.settings.merge_patch(args["settings"]);
             }
             if (args.contains("labels")) meta.labels = OptStrings(args, "labels");
             meta.source_hash.clear(); // never imported with these settings
             const bool import_now = OptBool(args, "import", true);
             auto command = std::make_unique<ImportSettingsCommand>(host, p.paths.file, guid, meta_file, std::move(before), meta, import_now);
             command->SetAttribution("agent");
             ImportSettingsCommand* command_result = command.get();
             editor::CommandContext context = session.Context();
             session.stack.Execute(context, std::move(command));
             Json out = {{"settings", meta.settings}, {"labels", meta.labels}};
             if (import_now) {
                 out["imported"] = command_result->Imported();
                 out["from_cache"] = command_result->FromCache();
                 out["warnings"] = command_result->Warnings();
                 if (!command_result->Imported()) out["error"] = command_result->ImportError();
             }
             return out;
         }});

    server.AddTool({"asset_move",
                    "Move or rename an asset or a whole folder (both relative to Content/) as one agent-attributed undoable action. GUID references stay unchanged; glTF relative URIs are rewritten.",
                    Schema({{"from", {{"type", "string"}}}, {"to", {{"type", "string"}}}}, {"from", "to"}), [host, &session](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        if (session.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before moving content");
                        auto command = std::make_unique<MoveContentCommand>(host, p.paths.file,
                            CleanRelative(RequireString(args, "from")), CleanRelative(RequireString(args, "to")));
                        command->SetAttribution("agent");
                        MoveContentCommand* command_result = command.get();
                        editor::CommandContext context = session.Context();
                        session.stack.Execute(context, std::move(command));
                        Json out = {{"moved", true}, {"rewritten", command_result->Result().rewritten}};
                        if (!command_result->Result().error.empty()) out["warning"] = command_result->Result().error;
                        return out;
                    }});

    server.AddTool({"asset_create_folder", "Create a folder under Content/ as one agent-attributed undoable action. Undo removes it only while it is empty.",
                    Schema({{"parent", {{"type", "string"}}}, {"name", {{"type", "string"}}}}, {"name"}), [host, &session](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        if (session.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before creating a content folder");
                        const std::string parent = CleanRelative(OptString(args, "parent"));
                        const std::string name = RequireString(args, "name");
                        auto command = std::make_unique<CreateContentFolderCommand>(host, p.paths.file, parent, name);
                        command->SetAttribution("agent");
                        editor::CommandContext context = session.Context();
                        session.stack.Execute(context, std::move(command));
                        return {{"created", true}};
                    }});

    server.AddTool({"asset_delete",
                    "Delete an asset's source and .ameta (with its sub-assets) as one agent-attributed undoable action. Refused while other assets reference it, unless `force`.",
                    Schema({{"asset", {{"type", "string"}}}, {"force", {{"type", "boolean"}}}}, {"asset"}), [host, &session](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        if (session.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before deleting an asset");
                        const AssetRecord& rec = FindAsset(p, RequireString(args, "asset"));
                        const std::string path = rec.path;
                        const assets::AssetGuid guid = rec.guid;
                        auto command = std::make_unique<DeleteAssetCommand>(host, p.paths.file, guid, p.paths.content / path,
                                                                            OptBool(args, "force", false));
                        command->SetAttribution("agent");
                        editor::CommandContext context = session.Context();
                        session.stack.Execute(context, std::move(command));
                        return {{"deleted", path}};
                    }});

    server.AddTool({"content_read", "Read a text file under Content/ (scenes, prefabs, materials, .ameta, JSON assets), up to 2 MB.",
                    Schema({{"path", {{"type", "string"}, {"description", "Relative to Content/"}}}}, {"path"}), [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        const fs::path file = ContentFile(p, RequireString(args, "path"));
                        std::error_code ec;
                        if (!fs::is_regular_file(file, ec)) throw ToolError("No such file: " + RequireString(args, "path"));
                        if (fs::file_size(file, ec) > kMaxTextBytes) throw ToolError("File is larger than 2 MB");
                        std::ifstream in(file, std::ios::binary);
                        std::stringstream ss;
                        ss << in.rdbuf();
                        const std::string text = ss.str();
                        if (text.find('\0') != std::string::npos) throw ToolError("Not a text file (contains NUL bytes)");
                        return {{"path", RequireString(args, "path")}, {"text", text}};
                    }});

    server.AddTool(
        {"content_write",
         "Write a text file under Content/ (a scene, prefab, material or any JSON asset) as one agent-attributed undoable action; "
         "new recognized assets get a GUID. Refuses .ameta files (use asset_set_import_settings) and existing files over 2 MB. Overwrites an existing file.",
         Schema({{"path", {{"type", "string"}}}, {"text", {{"type", "string"}}}}, {"path", "text"}), [host, &session](const Json& args) -> Json {
             OpenProject& p = host->Require();
             if (session.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before writing content");
             const std::string rel = RequireString(args, "path");
             const fs::path file = ContentFile(p, rel);
             if (file.extension() == assets::AssetMeta::kExtension) throw ToolError(".ameta files are managed by the asset tools");
             const std::string text = RequireString(args, "text");
             if (text.size() > kMaxTextBytes) throw ToolError("Text is larger than 2 MB");
             ContentFileState before_source = CaptureContentFile(file);
             ContentFileState before_meta = CaptureContentFile(fs::path(file.string() + assets::AssetMeta::kExtension));
             auto command = std::make_unique<ContentWriteCommand>(host, p.paths.file, file, text, std::move(before_source), std::move(before_meta));
             command->SetAttribution("agent");
             ContentWriteCommand* command_result = command.get();
             editor::CommandContext context = session.Context();
             session.stack.Execute(context, std::move(command));
             Json j = {{"path", CleanRelative(rel)}, {"bytes", text.size()}, {"scan", ScanJson(command_result->Scan())}};
             if (const AssetRecord* r = p.database->FindByPath(CleanRelative(rel))) j["asset"] = RecordJson(p, *r);
             return j;
         }});

    server.AddTool(
        {"asset_cook",
         "Cook the open project into an .apak archive (only what the startup scene, always_cook and `always_cook` reach, with "
         "textures block-compressed and meshes given LODs). Runs the importers as needed. The result's pak_file can be given to "
         "game_load. Blocks until done.",
         Schema({{"output_dir", {{"type", "string"}, {"description", "Gets <pak_name>.apak and CookManifest.json"}}},
                 {"configuration", {{"type", "string"}, {"description", "debug, development (default) or shipping"}}},
                 {"always_cook", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Extra roots: asset paths or folders ending in /"}}},
                 {"pak_name", {{"type", "string"}, {"description", "Default Game"}}},
                 {"compression", {{"type", "string"}, {"description", "auto (default), lz4, zstd or none"}}},
                 {"strict", {{"type", "boolean"}, {"description", "Refuse to write the archive if anything warns"}}},
                 {"include_imported", {{"type", "boolean"}}},
                 {"cook_textures", {{"type", "boolean"}}},
                 {"strip_source_textures", {{"type", "boolean"}, {"description", "Leave each cooked texture's source image out of the archive (the player decodes the cooked copy)"}}},
                 {"texture_target", {{"type", "string"}, {"description", "desktop (default: BC formats) or mobile (ASTC): what automatic texture format choice picks from"}}},
                 {"cook_meshes", {{"type", "boolean"}}},
                 {"key", {{"type", "string"}, {"description", "64 hex digits: encrypt the archive"}}},
                 {"patch_of", {{"type", "string"}, {"description", "Earlier .apak: write only what changed since"}}},
                 {"max_assets", {{"type", "integer"}, {"description", "How many cooked assets to list (default 100)"}}}},
                {"output_dir"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             cook::CookOptions o;
             o.project_file = p.paths.file;
             o.output_dir = RequireString(args, "output_dir");
             if (args.contains("configuration") && !cook::ParseConfiguration(OptString(args, "configuration"), o.configuration)) {
                 throw ToolError("\"configuration\" must be debug, development or shipping");
             }
             o.always_cook = OptStrings(args, "always_cook");
             o.pak_name = OptString(args, "pak_name", "Game");
             const std::string c = OptString(args, "compression", "auto");
             if (c == "lz4") o.compression = pak::CompressionPolicy::LZ4;
             else if (c == "zstd") o.compression = pak::CompressionPolicy::Zstd;
             else if (c == "none") o.compression = pak::CompressionPolicy::None;
             else if (c != "auto") throw ToolError("\"compression\" must be auto, lz4, zstd or none");
             o.strict_validation = OptBool(args, "strict", false);
             o.include_imported = OptBool(args, "include_imported", true);
             o.cook_textures = OptBool(args, "cook_textures", true);
             o.strip_source_textures = OptBool(args, "strip_source_textures", false);
             if (args.contains("texture_target") && !cook::ParseTextureTarget(OptString(args, "texture_target"), o.texture_target)) {
                 throw ToolError("\"texture_target\" must be desktop or mobile");
             }
             o.cook_meshes = OptBool(args, "cook_meshes", true);
             if (args.contains("key")) {
                 pak::PakKey key;
                 if (!pak::PakKey::FromHex(OptString(args, "key"), key)) throw ToolError("\"key\" must be 64 hex digits");
                 o.encryption_key = key;
             }
             if (args.contains("patch_of")) o.patch_base = OptString(args, "patch_of");

             const cook::CookReport r = cook::Cook(o);
             p.database->Scan(); // the cook may have made .ameta files
             if (!r.ok) {
                 Json fail = {{"ok", false}, {"error", KindOfCookFailure(r)}, {"warnings", r.warnings}};
                 return fail;
             }
             usize limit = 100;
             if (args.contains("max_assets") && args["max_assets"].is_number_integer()) limit = static_cast<usize>(std::max<long long>(0, args["max_assets"].get<long long>()));
             Json listed = Json::array();
             for (const cook::CookedAsset& a : r.assets) {
                 if (listed.size() >= limit) break;
                 listed.push_back({{"path", a.path}, {"importer", a.importer}, {"reason", a.reason}, {"bytes", a.bytes}, {"cooked", a.cooked}});
             }
             return {{"ok", true},
                     {"pak_file", r.pak_file.string()},
                     {"manifest_file", r.manifest_file.string()},
                     {"assets_cooked", r.assets.size()},
                     {"assets_skipped", r.skipped},
                     {"original_bytes", r.original_bytes},
                     {"pak_bytes", r.pak_bytes},
                     {"stripped_fields", r.stripped_fields},
                     {"texture_cache_hits", r.texture_cache_hits},
                     {"texture_cache_misses", r.texture_cache_misses},
                     {"plugins", r.plugins},
                     {"modules", r.modules},
                     {"warnings", r.warnings},
                     {"assets", listed},
                     {"is_patch", r.is_patch}};
         }});
}

std::shared_ptr<detail::AssetHost> MakeAssetHost() { return std::make_shared<detail::AssetHost>(); }

} // namespace aether::mcp
