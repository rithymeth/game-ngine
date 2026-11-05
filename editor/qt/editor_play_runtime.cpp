#include "editor_play_runtime.h"

#include "aether/assets/asset_database.h"
#include "aether/blueprint/system.h"
#include "aether/ai/bt_world.h"
#include "aether/ai/perception.h"
#include "aether/nav/components.h"
#include "aether/core/log.h"
#include "aether/ecs/world.h"
#include "aether/job/job_system.h"
#include "aether/physics/character.h"
#include "aether/physics/physics_scene.h"
#include "aether/physics/physics_world.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/lifecycle.h"
#if defined(AETHER_QT_EDITOR_SCRIPTING)
#include "aether/script/luau_host.h"
#include "aether/script/script_system.h"
#include "aether/script/ai_api.h"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

namespace aether::editor::qt {

namespace {

void LoadInputAssets(const std::filesystem::path& content_root, input::InputAssetLibrary& library) {
    if (content_root.empty()) return;
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(
        content_root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    const auto load_entry = [&](const std::filesystem::directory_entry& entry) {
        const std::string filename = entry.path().filename().string();
        std::error_code status_error;
        const std::filesystem::file_status status = entry.symlink_status(status_error);
        if (status_error) return;
        if (std::filesystem::is_directory(status)) {
            if (!filename.empty() && filename.front() == '.') it.disable_recursion_pending();
            return;
        }
        if (!std::filesystem::is_regular_file(status)) return;

        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const char* importer = extension == ".aaction" ? "InputAction"
            : extension == ".amapping" ? "InputMapping" : nullptr;
        if (!importer) return;

        std::ifstream file(entry.path(), std::ios::binary);
        if (!file) {
            AETHER_LOG_WARN("QtEditor", "Couldn't read input asset %s", entry.path().string().c_str());
            return;
        }
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const std::string relative = entry.path().lexically_relative(content_root).generic_string();
        library.AddFromText(importer, relative, text);
    };
    while (it != end) {
        load_entry(*it);
        it.increment(error);
        if (error) error.clear();
    }
}

} // namespace

EditorPlayRuntime::EditorPlayRuntime(World& world, GuidIndex& guids, const std::filesystem::path& content_root,
                                     const ProjectSettings& project,
                                     const std::vector<BlueprintBreakpoint>& breakpoints,
                                     std::function<bp::BpAction(const bp::BpStop&)> debug_handler,
                                     nav::NavWorld* navigation)
    : world_(world), fixed_step_(project.fixed_timestep_hz > 0.0f ? 1.0f / project.fixed_timestep_hz : 1.0f / 60.0f),
      blueprint_debug_handler_(std::move(debug_handler)), navigation_(navigation) {
    jobs_ = std::make_unique<JobSystem>();
    physics_ = std::make_unique<PhysicsWorld>(*jobs_);
    physics_->SetGravity(project.gravity);
    physics_->SetCollisionMatrix(MakeCollisionMatrix(project));
    scene_ = std::make_unique<PhysicsScene>(world_, *physics_);
    characters_ = std::make_unique<CharacterSystem>(world_, *physics_, scene_.get());
    lifecycle_ = std::make_unique<Lifecycle>(world_, guids);

    RegisterNavAgentComponents();
    RegisterPerceptionComponents();
    if (navigation_) {
        nav_crowd_ = std::make_unique<nav::NavCrowd>(world_, *navigation_);
        perception_ = std::make_unique<ai::PerceptionWorld>(world_);
        if (navigation_->Ready()) perception_->UseNavMeshLineOfSight(navigation_->Mesh());
    }

    bp::RegisterBlueprintComponents();
    if (!content_root.empty()) {
        assets_ = std::make_unique<assets::AssetDatabase>(content_root);
        const assets::ScanResult scan = assets_->Scan();
        for (const std::string& warning : scan.warnings)
            AETHER_LOG_WARN("QtEditor", "%s", warning.c_str());
    }
    blueprints_ = std::make_unique<bp::BlueprintSystem>(world_);
    blueprints_->SetLoader([this](const assets::AssetGuid& guid, bp::Blueprint& blueprint, std::string& name) {
        if (!assets_) return false;
        const assets::AssetRecord* record = assets_->Find(guid);
        if (!record || record->missing || record->importer != assets::BlueprintAsset::kImporter) return false;
        name = record->path;
        std::string error;
        return bp::LoadBlueprint(assets_->SourcePath(guid), blueprint, &error);
    });
    blueprints_->Register(*lifecycle_);
    blueprints_->VM().SetTraceEnabled(true, 8192);
    for (const auto& breakpoint : breakpoints) {
        if (!SetBreakpoint(breakpoint.asset, breakpoint.graph, breakpoint.node, true))
            AETHER_LOG_WARN("QtEditor", "Couldn't set Blueprint breakpoint at %s / node %u",
                            breakpoint.graph.c_str(), breakpoint.node);
    }

    behavior_trees_ = std::make_unique<ai::BehaviorTreeWorld>(world_, [this, content_root](const std::string& name) -> const ai::BehaviorTreeAsset* {
        auto found = behavior_tree_assets_.find(name);
        if (found != behavior_tree_assets_.end()) return &found->second;
        std::filesystem::path path(name);
        if (path.is_relative()) path = content_root / path;
        std::ifstream file(path, std::ios::binary);
        if (!file) return nullptr;
        nlohmann::json json;
        try { file >> json; } catch (...) { return nullptr; }
        ai::BehaviorTreeAsset tree;
        if (!ai::LoadBehaviorTree(json, tree)) return nullptr;
        tree.name = tree.name.empty() ? path.stem().string() : tree.name;
        return &behavior_tree_assets_.emplace(name, std::move(tree)).first->second;
    });
    behavior_trees_->hooks.log = [](Entity entity, const std::string& text) {
        AETHER_LOG_INFO("BehaviorTree", "Entity %u: %s", entity.index, text.c_str());
    };
    behavior_trees_->hooks.run_blueprint = [this](Entity entity, const std::string& event) {
        return blueprints_ && blueprints_->VM().Dispatch(entity, "Event.Custom:" + event);
    };
    behavior_trees_->hooks.abort_blueprint = [this](Entity entity, const std::string& event) {
        if (blueprints_) blueprints_->VM().Dispatch(entity, "Event.Custom:" + event + "Aborted");
    };
#if defined(AETHER_QT_EDITOR_SCRIPTING)
    behavior_trees_->hooks.run_luau = [this](Entity entity, const std::string& function, f32 dt, bool first) {
        if (!scripts_) return ai::BtStatus::Failure;
        const script::ScriptResult result = scripts_->CallMethod(entity, function,
            {static_cast<f64>(dt), first});
        if (!result.ok) {
            const std::string key = std::to_string(entity.index) + "/task/" + function;
            if (reported_bt_luau_errors_.insert(key).second)
                AETHER_LOG_WARN("BehaviorTree", "Luau task %s failed: %s", function.c_str(), result.error.c_str());
            return ai::BtStatus::Failure;
        }
        const auto* status = result.values.empty() ? nullptr : std::get_if<std::string>(&result.values.front());
        if (status && *status == "running") return ai::BtStatus::Running;
        if (status && *status == "success") return ai::BtStatus::Success;
        if (status && *status == "failure") return ai::BtStatus::Failure;
        const std::string key = std::to_string(entity.index) + "/task-result/" + function;
        if (reported_bt_luau_errors_.insert(key).second)
            AETHER_LOG_WARN("BehaviorTree", "Luau task %s must return 'running', 'success', or 'failure'", function.c_str());
        return ai::BtStatus::Failure;
    };
    behavior_trees_->hooks.luau_service = [this](Entity entity, const std::string& function, f32 dt) {
        if (!scripts_) return;
        const script::ScriptResult result = scripts_->CallMethod(entity, function, {static_cast<f64>(dt)});
        if (!result.ok) {
            const std::string key = std::to_string(entity.index) + "/service/" + function;
            if (reported_bt_luau_errors_.insert(key).second)
                AETHER_LOG_WARN("BehaviorTree", "Luau service %s failed: %s", function.c_str(), result.error.c_str());
        }
    };
#endif

    LoadInputAssets(content_root, input_library_);
    for (const std::string& warning : input_library_.Errors())
        AETHER_LOG_WARN("QtEditor", "%s", warning.c_str());
    input_library_.RegisterActions(input_);
    i32 input_priority = 0;
    for (const std::string& name : input_library_.ContextNames())
        input_library_.Activate(input_, name, input_priority++);

#if defined(AETHER_QT_EDITOR_SCRIPTING)
    host_ = std::make_unique<script::LuauHost>();
    scripts_ = std::make_unique<script::ScriptSystem>(
        *host_, world_, guids,
        [this](const assets::AssetGuid& guid, std::string& source, std::string& name) {
            if (!assets_) return false;
            const assets::AssetRecord* record = assets_->Find(guid);
            if (!record || record->missing || record->importer != assets::ScriptAsset::kImporter) return false;
            const std::filesystem::path path = assets_->SourcePath(guid);
            std::ifstream file(path, std::ios::binary);
            if (!file) return false;
            source.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            name = record->path;
            return true;
        }, false);
    scripts_->Register(*lifecycle_);
    scripts_->BindInput(&input_);
    scripts_->BindPhysicsScene(scene_.get());
    scene_->SetEventHandler([this](const PhysicsEvent& event) {
        if (!scripts_) return;
        const char* event_name = PhysicsEventName(event.type);
        constexpr char prefix[] = "Event.";
        const std::string full_name = event_name ? event_name : "";
        const std::string method = full_name.rfind(prefix, 0) == 0 ? full_name.substr(sizeof(prefix) - 1) : full_name;
        if (!method.empty()) {
            scripts_->SendEvent(event.self, method,
                {script::EntityRef{event.other}, static_cast<f64>(event.approach_speed)});
        }
    });
#else
    (void)content_root;
    (void)guids;
#endif

    scene_->Sync();
    lifecycle_->BeginPlay();
}

EditorPlayRuntime::~EditorPlayRuntime() { Stop(); }

void EditorPlayRuntime::Tick(f32 dt) {
    if (stopped_) return;
    dt = std::clamp(dt, 0.0f, 0.1f);
    input_.Update(input_state_, dt);
    lifecycle_->Update(dt);
    if (blueprints_) blueprints_->Update(dt);
    if (navigation_ && navigation_->Ready()) navigation_->Update(4);
    if (nav_crowd_) nav_crowd_->Update(dt);
    if (perception_) perception_->Update(dt);
    if (behavior_trees_) behavior_trees_->Update(dt);
#if defined(AETHER_QT_EDITOR_SCRIPTING)
    if (scripts_) scripts_->Tick(dt);
#endif

    accumulator_ += dt;
    constexpr int kMaxFixedStepsPerFrame = 8;
    int steps = 0;
    while (accumulator_ >= fixed_step_ && steps < kMaxFixedStepsPerFrame) {
        characters_->Step(fixed_step_);
        scene_->Step(fixed_step_);
        lifecycle_->FixedUpdate(fixed_step_);
        accumulator_ -= fixed_step_;
        ++steps;
    }
    if (steps == kMaxFixedStepsPerFrame && accumulator_ >= fixed_step_)
        accumulator_ = std::fmod(accumulator_, fixed_step_);
    lifecycle_->LateUpdate(dt);
    input_state_.EndFrame();
}

void EditorPlayRuntime::SetButton(input::Key key, bool down) {
    if (!stopped_) input_state_.SetButton(key, down);
}

void EditorPlayRuntime::AddMouseDelta(f32 dx, f32 dy) {
    if (!stopped_) input_state_.AddMouseDelta(dx, dy);
}

void EditorPlayRuntime::AddMouseWheel(f32 delta) {
    if (!stopped_) input_state_.AddWheel(delta);
}

void EditorPlayRuntime::ClearInput() {
    input_state_.Clear();
    input_.Reset();
}

bool EditorPlayRuntime::SetBreakpoint(const std::filesystem::path& asset, const std::string& graph,
                                     bp::NodeId node, bool enabled) {
    if (!assets_ || !blueprints_) return false;
    std::filesystem::path relative = asset.is_absolute() ? asset.lexically_relative(assets_->ContentRoot()) : asset;
    std::string asset_path = relative.generic_string();
    const assets::AssetRecord* record = assets_->FindByPath(asset_path);
    if (!record || record->missing || record->importer != assets::BlueprintAsset::kImporter) return false;
    const auto compiled = blueprints_->Get(record->guid);
    if (!compiled || !blueprints_->VM().SetBreakpoint(*compiled, graph, node, enabled)) return false;
    const auto key = std::make_tuple(compiled.get(), graph, node);
    if (enabled) active_breakpoints_.insert(key);
    else active_breakpoints_.erase(key);
    if (!active_breakpoints_.empty()) blueprints_->VM().SetDebugHandler(blueprint_debug_handler_);
    else blueprints_->VM().SetDebugHandler(nullptr);
    return true;
}

std::vector<std::tuple<std::filesystem::path, std::string, bp::NodeId>> EditorPlayRuntime::TakeBlueprintTrace() {
    std::vector<std::tuple<std::filesystem::path, std::string, bp::NodeId>> result;
    if (!blueprints_ || !assets_) return result;
    for (const bp::BpTraceEvent& event : blueprints_->VM().TakeTrace()) {
        const auto* instance = world_.GetComponent<bp::BlueprintInstance>(event.entity);
        if (!instance || !instance->blueprint.IsSet()) continue;
        const assets::AssetRecord* record = assets_->Find(instance->blueprint.guid);
        if (!record || record->missing || record->importer != assets::BlueprintAsset::kImporter) continue;
        result.emplace_back(assets_->ContentRoot() / std::filesystem::path(record->path), event.graph, event.node);
    }
    return result;
}

#if defined(AETHER_QT_EDITOR_SCRIPTING)
std::vector<script::AiDebugEntry> EditorPlayRuntime::AiDebugSnapshot() const {
    if (scripts_) return scripts_->AiDebug().Collect(world_);
    return {};
}
#endif

std::vector<BehaviorTreeDebugSnapshot> EditorPlayRuntime::BehaviorTreeDebugSnapshots() const {
    std::vector<BehaviorTreeDebugSnapshot> snapshots;
    if (!behavior_trees_) return snapshots;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<BehaviorTreeComponent>())) return;
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            Entity* entities = archetype.EntityArray(chunk);
            for (usize i = 0; i < archetype.ChunkEntityCount(chunk); ++i) {
                const Entity entity = entities[i];
                const auto* component = world_.GetComponent<BehaviorTreeComponent>(entity);
                if (!component) continue;
                BehaviorTreeDebugSnapshot snapshot;
                snapshot.entity = entity;
                snapshot.tree = component->tree;
                snapshot.running = component->running;
                snapshot.active_node = component->active;
                if (const ai::BehaviorTreeInstance* instance = behavior_trees_->InstanceOf(entity)) {
                    for (usize node_index = 0; node_index < instance->Nodes().size(); ++node_index) {
                        const auto& node = instance->Nodes()[node_index];
                        const bool active = instance->IsActive(node_index);
                        const char* result = instance->LastStatus(node_index) == ai::BtStatus::Success ? "Success"
                            : instance->LastStatus(node_index) == ai::BtStatus::Failure ? "Failure" : "Running";
                        snapshot.nodes.push_back({node.path, node.node ? node.node->Label() : "Node",
                                                 active ? "Running" : result, active});
                    }
                    for (const auto& key : instance->Tree().blackboard.keys) {
                        const ai::BlackboardValue* value = component->blackboard ? component->blackboard->Get(key.name) : nullptr;
                        snapshot.blackboard.push_back({key.name, ai::BlackboardTypeName(key.type),
                            value ? ai::ValueToString(*value) : "(unset)"});
                    }
                }
                snapshots.push_back(std::move(snapshot));
            }
        }
    });
    return snapshots;
}

bool EditorPlayRuntime::ControlBehaviorTree(Entity entity, const std::string& command) {
    auto* component = world_.GetComponent<BehaviorTreeComponent>(entity);
    if (!component) return false;
    if (command == "Start") component->Start();
    else if (command == "Stop") component->Stop();
    else if (command == "Restart") component->Restart();
    else return false;
    return true;
}

void EditorPlayRuntime::Stop() {
    if (stopped_) return;
    stopped_ = true;
    if (blueprints_) blueprints_->VM().SetDebugHandler(nullptr);
    ClearInput();
    if (lifecycle_ && lifecycle_->IsPlaying()) lifecycle_->EndPlay();
    behavior_trees_.reset();
    perception_.reset();
    nav_crowd_.reset();
    navigation_ = nullptr;
#if defined(AETHER_QT_EDITOR_SCRIPTING)
    if (scripts_) scripts_->BindInput(nullptr);
#endif
#if defined(AETHER_QT_EDITOR_SCRIPTING)
    scripts_.reset();
    host_.reset();
    assets_.reset();
    behavior_tree_assets_.clear();
#endif
    reported_bt_luau_errors_.clear();
    lifecycle_.reset();
    blueprints_.reset();
    assets_.reset();
    characters_.reset();
    scene_.reset();
    physics_.reset();
    jobs_.reset();
}

} // namespace aether::editor::qt
