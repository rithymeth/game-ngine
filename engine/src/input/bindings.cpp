#include "aether/input/bindings.h"

#include "aether/platform/filesystem.h"
#include "aether/reflection/serialize.h"

#include <algorithm>
#include <cmath>
#include <system_error>

namespace aether::input {

namespace stdfs = std::filesystem;

namespace {

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

// Files are {"$type": <kind>, "data": <reflected object>}.
template <typename T>
bool SaveTyped(const char* kind, const T& object, const stdfs::path& file, std::string* error) {
    reflect::Json json = {{"$type", kind}, {"data", reflect::ToJson(object)}};
    std::string text = json.dump(2);
    text.push_back('\n');
    std::error_code ec;
    stdfs::create_directories(file.parent_path(), ec);
    if (!fs::WriteFileBytes(file.string(), text.data(), text.size())) {
        SetError(error, "Couldn't write " + file.string());
        return false;
    }
    return true;
}

template <typename T>
bool LoadTyped(const char* kind, const stdfs::path& file, T& out, std::string* error) {
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes)) {
        SetError(error, "Couldn't read " + file.string());
        return false;
    }
    const reflect::Json json = reflect::Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded() || !json.is_object() || json.value("$type", "") != kind || !json.contains("data")) {
        SetError(error, file.string() + " isn't " + std::string(kind) + " data");
        return false;
    }
    T object{};
    if (!reflect::FromJson(object, json["data"])) {
        SetError(error, file.string() + " has malformed " + std::string(kind) + " data");
        return false;
    }
    out = std::move(object);
    return true;
}

} // namespace

bool SaveInputAction(const InputAction& action, const stdfs::path& file, std::string* error) {
    return SaveTyped("InputAction", action, file, error);
}
bool LoadInputAction(const stdfs::path& file, InputAction& out, std::string* error) {
    return LoadTyped("InputAction", file, out, error);
}
bool SaveMappingContext(const InputMappingContext& context, const stdfs::path& file, std::string* error) {
    return SaveTyped("InputMappingContext", context, file, error);
}
bool LoadMappingContext(const stdfs::path& file, InputMappingContext& out, std::string* error) {
    return LoadTyped("InputMappingContext", file, out, error);
}

usize InputAssetLibrary::Load(const assets::AssetDatabase& database) {
    actions_.clear();
    contexts_.clear();
    errors_.clear();
    for (const assets::AssetRecord* record : database.All()) {
        if (record->missing || record->IsSubAsset()) {
            continue;
        }
        const stdfs::path file = database.ContentRoot() / record->path;
        std::string error;
        if (record->importer == "InputAction") {
            InputAction action;
            if (!LoadInputAction(file, action, &error)) {
                errors_.push_back(error);
            } else if (action.name.empty()) {
                errors_.push_back(record->path + ": the action has no name");
            } else {
                actions_.push_back(std::move(action));
            }
        } else if (record->importer == "InputMapping") {
            InputMappingContext context;
            if (!LoadMappingContext(file, context, &error)) {
                errors_.push_back(error);
            } else {
                if (context.name.empty()) {
                    context.name = stdfs::path(record->path).stem().string();
                }
                contexts_.push_back(std::move(context));
            }
        }
    }
    return errors_.size();
}

const InputMappingContext* InputAssetLibrary::FindContext(const std::string& name) const {
    for (const InputMappingContext& context : contexts_) {
        if (context.name == name) {
            return &context;
        }
    }
    return nullptr;
}

std::vector<std::string> InputAssetLibrary::ContextNames() const {
    std::vector<std::string> names;
    for (const InputMappingContext& context : contexts_) {
        names.push_back(context.name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void InputAssetLibrary::RegisterActions(InputSystem& system) const {
    for (const InputAction& action : actions_) {
        system.AddAction(action);
    }
}

bool InputAssetLibrary::Activate(InputSystem& system, const std::string& name, i32 priority,
                                 const UserBindings* user) const {
    const InputMappingContext* context = FindContext(name);
    if (context == nullptr) {
        return false;
    }
    system.AddContext(user != nullptr ? ApplyUserBindings(*context, *user) : *context, priority);
    return true;
}

void UserBindings::Set(const std::string& context, const std::string& action, u32 slot, Key key) {
    for (KeyOverride& o : overrides) {
        if (o.context == context && o.action == action && o.slot == slot) {
            o.key = key;
            return;
        }
    }
    overrides.push_back({context, action, slot, key});
}

bool UserBindings::Reset(const std::string& context, const std::string& action, u32 slot) {
    auto it = std::find_if(overrides.begin(), overrides.end(), [&](const KeyOverride& o) {
        return o.context == context && o.action == action && o.slot == slot;
    });
    if (it == overrides.end()) {
        return false;
    }
    overrides.erase(it);
    return true;
}

const KeyOverride* UserBindings::Find(const std::string& context, const std::string& action, u32 slot) const {
    for (const KeyOverride& o : overrides) {
        if (o.context == context && o.action == action && o.slot == slot) {
            return &o;
        }
    }
    return nullptr;
}

i32 FindBindingSlot(const InputMappingContext& context, const std::string& action, u32 slot) {
    u32 seen = 0;
    for (usize i = 0; i < context.bindings.size(); ++i) {
        if (context.bindings[i].action == action) {
            if (seen == slot) {
                return static_cast<i32>(i);
            }
            ++seen;
        }
    }
    return -1;
}

InputMappingContext ApplyUserBindings(const InputMappingContext& context, const UserBindings& user,
                                      std::vector<KeyOverride>* stale) {
    InputMappingContext result = context;
    for (const KeyOverride& o : user.overrides) {
        if (o.context != context.name) {
            continue;
        }
        const i32 index = FindBindingSlot(result, o.action, o.slot);
        if (index < 0) {
            if (stale != nullptr) {
                stale->push_back(o);
            }
            continue;
        }
        result.bindings[static_cast<usize>(index)].key = o.key;
    }
    return result;
}

std::vector<std::pair<std::string, u32>> FindConflicts(const InputMappingContext& context, Key key,
                                                       const std::string& except_action, u32 except_slot) {
    std::vector<std::pair<std::string, u32>> conflicts;
    if (key == Key::None) {
        return conflicts;
    }
    std::map<std::string, u32> slots;
    for (const InputBinding& binding : context.bindings) {
        const u32 slot = slots[binding.action]++;
        if (binding.key == key && !(binding.action == except_action && slot == except_slot)) {
            conflicts.emplace_back(binding.action, slot);
        }
    }
    return conflicts;
}

bool SaveUserBindings(const UserBindings& user, const stdfs::path& file, std::string* error) {
    return SaveTyped("UserBindings", user, file, error);
}

bool LoadUserBindings(const stdfs::path& file, UserBindings& out, std::string* error) {
    std::error_code ec;
    if (!stdfs::exists(file, ec)) {
        out = UserBindings{};
        return true;
    }
    return LoadTyped("UserBindings", file, out, error);
}

stdfs::path UserBindingsPath(const stdfs::path& saved_dir) { return saved_dir / "Config" / "Input.json"; }

void KeyCapture::Begin(const InputState& state, bool escape_cancels) {
    active_ = true;
    escape_cancels_ = escape_cancels;
    held_at_start_.assign(kKeyCount, false);
    for (usize i = 0; i < kKeyCount; ++i) {
        held_at_start_[i] = std::fabs(state.Value(static_cast<Key>(i))) >= 0.5f;
    }
}

KeyCapture::Result KeyCapture::Update(const InputState& state, Key& key) {
    if (!active_) {
        return Result::Canceled;
    }
    for (usize i = 1; i < kKeyCount; ++i) {
        const Key candidate = static_cast<Key>(i);
        if (IsDelta(candidate)) {
            continue; // mouse movement and wheel
        }
        const bool down = std::fabs(state.Value(candidate)) >= 0.5f;
        if (held_at_start_[i]) {
            held_at_start_[i] = down; // must be released first
            continue;
        }
        if (!down) {
            continue;
        }
        if (candidate == Key::Escape && escape_cancels_) {
            active_ = false;
            return Result::Canceled;
        }
        key = candidate;
        active_ = false;
        return Result::Captured;
    }
    return Result::Waiting;
}

} // namespace aether::input
