// Script hot reload (Phase 11 step 5, docs/design/PHASE_SPECS.md §11.3).

#include "aether/script/script_system.h"

#include "aether/core/log.h"
#include "aether/platform/filesystem.h"
#include "aether/scene/gameplay.h"

#include <lua.h>
#include <lualib.h>

#include <cctype>
#include <filesystem>

namespace aether::script {

ScriptError ScriptError::Parse(const std::string& text) {
    ScriptError error;
    error.text = text;
    error.message = text;
    // "<chunk>:<line>: <message>"; the chunk may itself contain ':' (a Windows path).
    for (usize colon = text.find(':'); colon != std::string::npos; colon = text.find(':', colon + 1)) {
        usize digits = colon + 1;
        while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits]))) {
            ++digits;
        }
        if (digits > colon + 1 && digits < text.size() && text[digits] == ':') {
            error.chunk = text.substr(0, colon);
            error.line = std::stoi(text.substr(colon + 1, digits - colon - 1));
            error.message = text.substr(digits + 1);
            if (!error.message.empty() && error.message.front() == ' ') {
                error.message.erase(0, 1);
            }
            break;
        }
    }
    return error;
}

ScriptSystem::SourceLoader ScriptSystem::DatabaseLoader(const assets::AssetDatabase& database) {
    return [&database](const assets::AssetGuid& guid, std::string& source, std::string& name) {
        const assets::AssetRecord* record = database.Find(guid);
        if (record == nullptr || record->missing || record->importer != "Script") {
            return false;
        }
        std::vector<u8> bytes;
        if (!fs::ReadFileBytes(database.SourcePath(guid).string(), bytes)) {
            return false;
        }
        source.assign(bytes.begin(), bytes.end());
        name = std::filesystem::path(record->path).filename().string();
        return true;
    };
}

ScriptSystem::ReloadResult ScriptSystem::Reload(const assets::AssetGuid& script) {
    ReloadResult result;
    std::string source;
    std::string name;
    if (!loader_ || !loader_(script, source, name)) {
        result.error = "Script " + assets::ToString(script) + " isn't available";
        reload_errors_[script] = ScriptError::Parse(result.error);
        return result;
    }
    int new_ref = LUA_NOREF;
    ScriptClassInfo info = DescribeSource(host_, source, name, &new_ref);
    if (!info.ok) {
        // Keep running the old version; tell the user what's wrong.
        result.error = info.error;
        reload_errors_[script] = ScriptError::Parse(info.error);
        RecordError(info.error + " (reload failed; the previous version keeps running)");
        return result;
    }
    reload_errors_.erase(script);

    lua_State* L = host_.State();
    lua_createtable(L, 0, 1);
    lua_getref(L, new_ref);
    lua_setfield(L, -2, "__index");
    const int new_meta = lua_ref(L, -1);
    lua_pop(L, 1);

    // Swap the class under every live instance. Instance fields (state,
    // overridden variables) are on the instance table, so they carry over.
    std::vector<EntityGuid> reloaded;
    for (auto& [guid, instance] : instances_) {
        if (instance.script != script) {
            continue;
        }
        lua_getref(L, instance.ref);
        lua_getref(L, new_meta);
        lua_setmetatable(L, -2);
        lua_pop(L, 1);
        instance.name = name;
        reloaded.push_back(guid);
    }

    // Replace the cached class (the old tables go once nothing uses them).
    auto it = classes_.find(script);
    if (it != classes_.end()) {
        if (it->second.ref != LUA_NOREF) {
            lua_unref(L, it->second.ref);
        }
        if (it->second.meta_ref != LUA_NOREF) {
            lua_unref(L, it->second.meta_ref);
        }
    }
    Class& cls = classes_[script];
    cls.ref = new_ref;
    cls.meta_ref = new_meta;
    cls.name = name;
    cls.info = std::move(info);

    for (const EntityGuid& guid : reloaded) {
        const Entity entity = guids_.Find(world_, guid);
        if (!entity.IsNull()) {
            Invoke(entity, "OnReload");
        }
    }
    // Entities that entered play while their script couldn't load (it had an
    // error) start running now that it can.
    std::vector<Entity> waiting;
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        const Entity entity = guids_.Find(world_, it->first);
        if (entity.IsNull()) {
            it = waiting_.erase(it); // gone meanwhile
        } else if (it->second == script) {
            waiting.push_back(entity);
            it = waiting_.erase(it);
        } else {
            ++it;
        }
    }
    for (Entity entity : waiting) {
        Create(entity);
        Invoke(entity, "OnCreate");
        if (IsActiveInHierarchy(world_, guids_, entity)) {
            Invoke(entity, "OnEnable");
        }
        Invoke(entity, "OnStart");
    }
    result.ok = true;
    result.instances = reloaded.size();
    AETHER_LOG_INFO("Script", "Reloaded %s (%zu instance(s))", name.c_str(), reloaded.size());
    return result;
}

usize ScriptSystem::ReloadChanged(const std::vector<assets::AssetChange>& changes) {
    usize attempted = 0;
    for (const assets::AssetChange& change : changes) {
        const bool changed = change.kind == assets::AssetChange::Kind::Changed ||
                             change.kind == assets::AssetChange::Kind::Reimported;
        if (!changed || std::filesystem::path(change.path).extension() != ".luau") {
            continue;
        }
        // Only scripts that are loaded need reloading; others load fresh when first used.
        if (classes_.count(change.guid) == 0 && reload_errors_.count(change.guid) == 0) {
            continue;
        }
        Reload(change.guid);
        ++attempted;
    }
    return attempted;
}

} // namespace aether::script
