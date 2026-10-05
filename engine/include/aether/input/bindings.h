#pragma once

#include "aether/assets/asset_database.h"
#include "aether/input/actions.h"

#include <filesystem>
#include <string_view>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aether::input {

struct UserBindings;

// ---------------------------------------------------------------------------
// Input assets (Phase 10 step 2)
// ---------------------------------------------------------------------------

// Input actions (.aaction) and mapping contexts (.amapping) are reflected
// JSON files in the content folder, found through the AssetDatabase
// (importers "InputAction" / "InputMapping").
bool SaveInputAction(const InputAction& action, const std::filesystem::path& file, std::string* error = nullptr);
bool LoadInputAction(const std::filesystem::path& file, InputAction& out, std::string* error = nullptr);
bool SaveMappingContext(const InputMappingContext& context, const std::filesystem::path& file, std::string* error = nullptr);
bool LoadMappingContext(const std::filesystem::path& file, InputMappingContext& out, std::string* error = nullptr);

// Every input asset in a project, loaded. Bindings refer to actions by name,
// so asset names (not paths) are what matter.
class InputAssetLibrary {
public:
    // (Re)loads every .aaction and .amapping the database knows. Returns the
    // number of files that couldn't be read; see Errors().
    usize Load(const assets::AssetDatabase& database);
    const std::vector<std::string>& Errors() const { return errors_; }
    // Adds one asset from its text, for assets that don't come from a
    // database on disk (a packaged game reads them from its archives).
    // `importer` is "InputAction" or "InputMapping"; `path` names it in
    // messages (and a nameless context after its file). A bad one is also in Errors().
    bool AddFromText(const std::string& importer, const std::string& path, std::string_view text,
                     std::string* error = nullptr);

    const std::vector<InputAction>& Actions() const { return actions_; }
    const InputMappingContext* FindContext(const std::string& name) const;
    std::vector<std::string> ContextNames() const;

    // Registers every action with `system` and activates the named context
    // (with the player's rebinds applied, if given). False if there's no
    // such context.
    void RegisterActions(InputSystem& system) const;
    bool Activate(InputSystem& system, const std::string& context, i32 priority,
                  const UserBindings* user = nullptr) const;

private:
    std::vector<InputAction> actions_;
    std::vector<InputMappingContext> contexts_;
    std::vector<std::string> errors_;
};

// ---------------------------------------------------------------------------
// Rebinding
// ---------------------------------------------------------------------------

// One player rebind: the `slot`-th binding of `action` in `context` (counting
// only that action's bindings, in order) now uses `key`. Key::None unbinds it.
struct KeyOverride {
    std::string context;
    std::string action;
    u32 slot = 0;
    Key key = Key::None;
};

// A player's rebinds, saved separately from the project's defaults (in
// the player's settings, §28.7) so defaults can change without losing them.
struct UserBindings {
    std::vector<KeyOverride> overrides;

    // Sets or replaces the rebind for (context, action, slot).
    void Set(const std::string& context, const std::string& action, u32 slot, Key key);
    // Back to the default; true if there was a rebind.
    bool Reset(const std::string& context, const std::string& action, u32 slot);
    void ResetAll() { overrides.clear(); }
    const KeyOverride* Find(const std::string& context, const std::string& action, u32 slot) const;
};

// The index into context.bindings of `action`'s `slot`-th binding, or -1.
i32 FindBindingSlot(const InputMappingContext& context, const std::string& action, u32 slot);

// A copy of `context` with the player's rebinds for it applied. Rebinds
// whose binding no longer exists are ignored (and listed in `stale`).
InputMappingContext ApplyUserBindings(const InputMappingContext& context, const UserBindings& user,
                                      std::vector<KeyOverride>* stale = nullptr);

// Other bindings in the context that already use `key` (for "already bound
// to Jump" warnings), as (action, slot) pairs; the binding being rebound is
// excluded.
std::vector<std::pair<std::string, u32>> FindConflicts(const InputMappingContext& context, Key key,
                                                       const std::string& except_action = "", u32 except_slot = 0);

bool SaveUserBindings(const UserBindings& user, const std::filesystem::path& file, std::string* error = nullptr);
// A missing file is not an error: no rebinds.
bool LoadUserBindings(const std::filesystem::path& file, UserBindings& out, std::string* error = nullptr);
// Where a project keeps a player's rebinds.
std::filesystem::path UserBindingsPath(const std::filesystem::path& saved_dir);

// ---------------------------------------------------------------------------
// "Press a key..." capture
// ---------------------------------------------------------------------------

// Waits for the player to press something to bind. Keys already held when
// capture begins are ignored until released; sticks and triggers count
// once pushed past half-way; mouse movement never counts (it's too easy to
// trigger by accident). Escape cancels (unless `escape_cancels` is false).
class KeyCapture {
public:
    void Begin(const InputState& state, bool escape_cancels = true);
    void Cancel() { active_ = false; }
    bool IsActive() const { return active_; }

    enum class Result { Waiting, Captured, Canceled };
    // Call once per frame while active. On Captured, `key` is set and
    // capture ends.
    Result Update(const InputState& state, Key& key);

private:
    bool active_ = false;
    bool escape_cancels_ = true;
    std::vector<bool> held_at_start_;
};

} // namespace aether::input

AETHER_REFLECT(aether::input::KeyOverride, 1, AETHER_FIELD(context), AETHER_FIELD(action), AETHER_FIELD(slot), AETHER_FIELD(key))
AETHER_REFLECT(aether::input::UserBindings, 1, AETHER_FIELD(overrides))
