#include "aether/player/bindings_migrate.h"

#include "aether/input/bindings.h"

#include <system_error>

namespace aether::player {

namespace stdfs = std::filesystem;

std::vector<std::string> MigrateLegacyBindings(const stdfs::path& saved_dir, save::SettingsStore<save::GameSettings>& store) {
    std::vector<std::string> warnings;
    const stdfs::path file = input::UserBindingsPath(saved_dir);
    std::error_code ec;
    if (!stdfs::exists(file, ec)) return warnings;
    input::UserBindings legacy;
    std::string error;
    if (!input::LoadUserBindings(file, legacy, &error)) {
        warnings.push_back("The old key bindings in " + file.string() + " couldn't be read (" + error + "); they were left as they are");
        return warnings;
    }
    if (store.Get().bindings.overrides.empty()) {
        save::GameSettings next = store.Get();
        next.bindings = std::move(legacy);
        store.Set(next);
    } else {
        warnings.push_back("The settings already have key bindings, so the old ones in " + file.string() + " were set aside");
    }
    stdfs::path moved = file;
    moved += ".migrated";
    stdfs::rename(file, moved, ec);
    if (ec) warnings.push_back("Couldn't rename " + file.string() + " after moving its bindings: " + ec.message());
    return warnings;
}

} // namespace aether::player
