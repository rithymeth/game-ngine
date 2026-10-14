#pragma once

#include "aether/input/actions.h"
#include "aether/input/bindings.h"
#include "aether/project/project.h"
#include "aether/blueprint/vm.h"
#include "aether/ai/bt_world.h"
#include "aether/ai/debug_draw.h"
#include "aether/nav/crowd.h"
#include "behavior_tree_debug.h"

#include <filesystem>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_set>
#include <unordered_map>

namespace aether {
namespace bp { class BlueprintSystem; }
class CharacterSystem;
class GuidIndex;
class JobSystem;
class Lifecycle;
class PhysicsScene;
class PhysicsWorld;
class World;
namespace nav { class NavWorld; }
namespace assets { class AssetDatabase; }
namespace script {
class LuauHost;
class ScriptSystem;
}
}

namespace aether::editor::qt {

struct BlueprintBreakpoint {
    std::filesystem::path asset;
    std::string graph;
    bp::NodeId node = 0;
};

// Scene-scoped runtime used only while the Qt editor is in Play mode.
class EditorPlayRuntime final {
public:
    EditorPlayRuntime(World& world, GuidIndex& guids, const std::filesystem::path& content_root,
                      const ProjectSettings& project,
                      const std::vector<BlueprintBreakpoint>& breakpoints = {},
                      std::function<bp::BpAction(const bp::BpStop&)> debug_handler = {},
                      nav::NavWorld* navigation = nullptr);
    ~EditorPlayRuntime();

    EditorPlayRuntime(const EditorPlayRuntime&) = delete;
    EditorPlayRuntime& operator=(const EditorPlayRuntime&) = delete;

    void Tick(f32 dt);
    void SetButton(input::Key key, bool down);
    void AddMouseDelta(f32 dx, f32 dy);
    void AddMouseWheel(f32 delta);
    void ClearInput();
    void Stop();
    bool SetBreakpoint(const std::filesystem::path& asset, const std::string& graph, bp::NodeId node, bool enabled);
    std::vector<std::tuple<std::filesystem::path, std::string, bp::NodeId>> TakeBlueprintTrace();
    std::vector<BehaviorTreeDebugSnapshot> BehaviorTreeDebugSnapshots() const;
    bool ControlBehaviorTree(Entity entity, const std::string& command);
    const nav::NavCrowd* NavigationCrowd() const { return nav_crowd_.get(); }

private:
    World& world_;
    input::InputAssetLibrary input_library_;
    input::InputSystem input_;
    input::InputState input_state_;
    f32 fixed_step_ = 1.0f / 60.0f;
    f32 accumulator_ = 0.0f;
    bool stopped_ = false;

    std::unique_ptr<JobSystem> jobs_;
    std::unique_ptr<PhysicsWorld> physics_;
    std::unique_ptr<PhysicsScene> scene_;
    std::unique_ptr<CharacterSystem> characters_;
    std::unique_ptr<Lifecycle> lifecycle_;
    std::unique_ptr<assets::AssetDatabase> assets_;
    std::unique_ptr<bp::BlueprintSystem> blueprints_;
    std::unique_ptr<ai::BehaviorTreeWorld> behavior_trees_;
    nav::NavWorld* navigation_ = nullptr;
    std::unique_ptr<nav::NavCrowd> nav_crowd_;
    std::unique_ptr<ai::PerceptionWorld> perception_;
    std::unordered_map<std::string, ai::BehaviorTreeAsset> behavior_tree_assets_;
    std::function<bp::BpAction(const bp::BpStop&)> blueprint_debug_handler_;
    std::unordered_set<std::string> reported_bt_luau_errors_;
    std::set<std::tuple<const bp::CompiledBlueprint*, std::string, bp::NodeId>> active_breakpoints_;
#if defined(AETHER_QT_EDITOR_SCRIPTING)
    std::unique_ptr<script::LuauHost> host_;
    std::unique_ptr<script::ScriptSystem> scripts_;
#endif
};

} // namespace aether::editor::qt
