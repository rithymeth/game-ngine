#pragma once

// MCP tools that drive the editor's scene: list, inspect, create, edit and
// delete entities and components, reparent, undo/redo, save/load scenes and
// Play-in-Editor. Every change goes through the editor's undoable commands
// (editor/src/core/commands.h), so what an AI agent does can be undone with
// the same history a human uses.
//
// The session owns a headless editor world: no window, graphics or ImGui, so
// the server runs anywhere the engine core builds.

#include "aether/ecs/world.h"
#include "aether/scene/entity_guid.h"
#include "core/command_stack.h"
#include "core/play_session.h"
#include "mcp_server.h"
#include "simulation.h"

#include <filesystem>
#include <functional>
#include <memory>

namespace aether::mcp {

struct EditorSession {
    World world;
    GuidIndex guids;
    editor::EditorHooks hooks;
    editor::CommandStack stack;
    std::function<const PrefabData*(const assets::AssetGuid&)> prefab_lookup;
    editor::PlaySession play;
    // Exists from Play to Stop; runs the physics systems (see simulation.h).
    std::unique_ptr<Simulation> sim;
    // save_scene and load_scene stay inside this folder (relative paths resolve against it; symlinks and ".." cannot
    // leave it). Empty means unrestricted, for embedding and tests; the server program sets it (see --scene-root).
    std::filesystem::path scene_root;

    editor::CommandContext Context() {
        editor::CommandContext context(world, guids, &hooks);
        context.find_prefab = prefab_lookup;
        return context;
    }
};

// Makes sure the engine's built-in scene components (Transform, Parent,
// IdComponent, ModelRenderer) are registered, so tools can find them by name
// before any entity has used them. Components from other modules appear once
// the program registers them (GetComponentId<T>()).
void RegisterBuiltinComponents();

// The file a scene tool may touch for `requested`, or an empty path with `error` set: only .json and .aesc files, and
// (when `root` is set) only inside `root`.
std::filesystem::path ResolveScenePath(const std::filesystem::path& root, const std::string& requested, std::string& error);

// Adds the editor tools (see the .cpp for the list) to `server`, operating on
// `session`, which must outlive the server.
void RegisterEditorTools(McpServer& server, EditorSession& session);

} // namespace aether::mcp
