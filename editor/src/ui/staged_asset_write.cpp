#include "ui/staged_asset_write.h"

#include <atomic>
#include <chrono>
#include <fstream>

namespace aether::editor {

namespace {

namespace fs = std::filesystem;

// Reserve a unique staging directory next to the destination. A reused
// temp filename could collide with another editor saving in the same folder.
class AssetWriteStage {
public:
    explicit AssetWriteStage(const fs::path& target) {
        const fs::path parent = target.has_parent_path() ? target.parent_path() : fs::path(".");
        static std::atomic<unsigned long long> serial{0};
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 32; ++attempt) {
            const fs::path candidate = parent / (".aether-asset-save-" + std::to_string(now) + "-" +
                std::to_string(serial.fetch_add(1, std::memory_order_relaxed)));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) {
                directory_ = candidate;
                return;
            }
            if (ec && ec != std::errc::file_exists) return;
        }
    }

    AssetWriteStage(const AssetWriteStage&) = delete;
    AssetWriteStage& operator=(const AssetWriteStage&) = delete;

    ~AssetWriteStage() {
        if (directory_.empty()) return;
        std::error_code ec;
        fs::remove(File(), ec);
        fs::remove(directory_, ec);
    }

    bool Ready() const { return !directory_.empty(); }
    fs::path File() const { return directory_ / "asset"; }

private:
    fs::path directory_;
};

} // namespace

bool WriteStagedAsset(const fs::path& path, std::string_view contents) {
    const AssetWriteStage stage(path);
    if (!stage.Ready()) return false;
    const auto staged_file = stage.File();
    std::ofstream file(staged_file, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.close(); // A buffered write may first fail here.
    if (!file) return false;
    std::error_code ec;
    fs::rename(staged_file, path, ec);
    return !ec;
}

} // namespace aether::editor
