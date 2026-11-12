#include "host/live_viewport.h"

#include "aether/core/log.h"
#include "aether/player/game.h"
#include "aether/player/scene_renderer.h"
#include "aether/scene/components.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/script_component.h"
#include "aether/scene/prefab.h"
#include "core/commands.h"
#include "aether/renderer/components.h"
#include "aether/renderer/material_instance.h"
#include "aether/scene/gameplay.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace aether::editor {

namespace {

struct GuidHash {
    usize operator()(const EntityGuid& g) const noexcept { return static_cast<usize>(g.hi ^ (g.lo * 0x9e3779b97f4a7c15ull)); }
};

// Every identified entity of a world, by GUID.
std::unordered_map<EntityGuid, Entity, GuidHash> IdentifiedEntities(const World& world) {
    std::unordered_map<EntityGuid, Entity, GuidHash> found;
    world.ForEachArchetype([&](const Archetype& archetype) {
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const Entity* array = archetype.EntityArray(chunk);
            for (u32 row = 0; row < archetype.ChunkEntityCount(chunk); ++row) {
                const IdComponent* id = world.GetComponent<IdComponent>(array[row]);
                if (id != nullptr && !id->guid.IsNull()) found[id->guid] = array[row];
            }
        }
    });
    return found;
}

} // namespace

// Members are declared in the order they must be destroyed in reverse: the renderer and the swap chain before the game, the game before the
// package it reads.
struct LiveViewport::Impl {
    player::GamePackage package;
    std::unique_ptr<player::Game> game;
    std::unique_ptr<rhi::ISwapChain> swap_chain;
    std::vector<std::unique_ptr<rhi::ICommandList>> commands;
    std::vector<u64> fences;
    std::unique_ptr<player::SceneRenderer> renderer;
    u64 frames = 0;
    u64 reloads = 0;
    u64 patches = 0;
    u64 script_restarts = 0;
    std::unordered_set<EntityGuid, GuidHash> known; // the identified entities the document had when the scene was last loaded or patched
    std::unordered_map<EntityGuid, Transform, GuidHash> synced; // their Transforms as the document had them then: a patch copies only the ones that changed since
    std::string scene;
};

LiveViewport::LiveViewport(rhi::IDevice& device) : device_(device) {}

LiveViewport::~LiveViewport() { Unload(); }

void LiveViewport::Unload() {
    if (!impl_) return;
    for (const u64 fence : impl_->fences) device_.WaitForFence(fence); // the GPU is done with everything before it is freed
    impl_.reset();
    pixels_.clear();
}

u32 LiveViewport::Width() const { return impl_ && impl_->swap_chain ? impl_->swap_chain->Width() : 0; }
u32 LiveViewport::Height() const { return impl_ && impl_->swap_chain ? impl_->swap_chain->Height() : 0; }
u64 LiveViewport::FrameCount() const { return impl_ ? impl_->frames : 0; }
u64 LiveViewport::SceneReloads() const { return impl_ ? impl_->reloads : 0; }
u64 LiveViewport::PatchCount() const { return impl_ ? impl_->patches : 0; }
u64 LiveViewport::ScriptRestarts() const { return impl_ ? impl_->script_restarts : 0; }
usize LiveViewport::EntityCount() const { return impl_ && impl_->game ? impl_->game->GetWorld().EntityCount() : 0; }
usize LiveViewport::ScriptInstanceCount() const { return impl_ && impl_->game ? impl_->game->Stats().script_instances : 0; }
usize LiveViewport::PhysicsBodyCount() const { return impl_ && impl_->game ? impl_->game->Stats().physics_bodies : 0; }

bool LiveViewport::SetScene(const std::vector<u8>& scene_bytes, std::string* error) {
    if (!impl_) {
        if (error) *error = "Nothing is loaded.";
        return false;
    }
    impl_->package.SetContentOverride(impl_->scene, scene_bytes);
    std::string problem;
    if (!impl_->game->LoadScene(impl_->scene, &problem)) {
        if (error) *error = problem;
        return false;
    }
    ++impl_->reloads;
    RememberKnown();
    return true;
}

void LiveViewport::RememberKnown() {
    impl_->known.clear();
    impl_->synced.clear();
    const World& world = impl_->game->GetWorld();
    for (const auto& entry : IdentifiedEntities(world)) {
        impl_->known.insert(entry.first);
        if (const Transform* transform = world.GetComponent<Transform>(entry.second)) impl_->synced[entry.first] = *transform; // just loaded: the document's
    }
}

bool LiveViewport::PositionOf(const EntityGuid& entity, Vec3& position) const {
    if (!impl_ || !impl_->game) return false;
    const World& world = impl_->game->GetWorld();
    const auto entities = IdentifiedEntities(world); // one map, kept for the lookup
    const auto found = entities.find(entity);
    if (found == entities.end()) return false;
    const Transform* transform = world.GetComponent<Transform>(found->second);
    if (transform == nullptr) return false;
    position = transform->position;
    return true;
}

namespace {

// The components the game's physics scene turns into bodies. It keeps them in step with the world itself (PhysicsScene::Sync, on every physics step: a new entity
// gets a body, a changed collider or rigid body is rebuilt, a gone entity's body is destroyed), so putting one into the running world, taking it out or changing
// it needs no reload. Found by name so the editor does not link the physics module.
bool IsPhysicsBodyComponent(ComponentId component) {
    const ComponentInfo& info = GetComponentInfo(component);
    if (info.reflected == nullptr) return false;
    const std::string name = info.reflected->name;
    return name == "RigidBody" || name == "BoxCollider" || name == "SphereCollider" || name == "CapsuleCollider" || name == "ConvexCollider" || name == "MeshCollider";
}

} // namespace

bool LiveViewport::IsPatchable(ComponentId component) {
    // What the renderer reads each frame (a MaterialParameters per draw: player/src/material_overrides.h) and what the physics scene keeps in step with the world.
    // A script is made and unmade by the game's lifecycle as its component comes and goes (and an edit restarts it: see PatchScene); a character controller or an audio
    // source is set up once when the game starts, so those need the scene loaded again. (A camera is found in the world every frame, like a light.)
    return IsPhysicsBodyComponent(component) || component == GetComponentId<Transform>() || component == GetComponentId<EntityName>() || component == GetComponentId<ModelRenderer>() ||
           component == GetComponentId<DirectionalLight>() || component == GetComponentId<PointLight>() || component == GetComponentId<SpotLight>() ||
           component == GetComponentId<SkyLight>() || component == GetComponentId<Wind>() || component == GetComponentId<WindZone>() ||
           component == GetComponentId<PostProcessVolume>() || component == GetComponentId<Decal>() || component == GetComponentId<MaterialParameters>() || component == GetComponentId<Parent>() ||
           component == GetComponentId<Camera>() || component == GetComponentId<ScriptComponent>();
}

namespace {

// What an entity may carry to be added to, or taken from, a running scene whole: the patchable components and the bookkeeping that ties it to
// the scene and to a prefab.
bool IsSpawnable(ComponentId component) {
    return LiveViewport::IsPatchable(component) || component == GetComponentId<IdComponent>() || component == GetComponentId<Parent>() ||
           component == GetComponentId<PrefabInstance>() || component == GetComponentId<PrefabLink>();
}

bool AllSpawnable(const World& world, Entity entity) {
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        if (world.HasComponentRaw(entity, id) && !IsSpawnable(id)) return false;
    }
    return true;
}

} // namespace

bool LiveViewport::PatchScene(const World& source, const std::vector<ComponentEdit>& edits) {
    if (!impl_ || !impl_->game) return false;
    World& running = impl_->game->GetWorld();
    const auto in_source = IdentifiedEntities(source);
    const auto in_running = IdentifiedEntities(running);
    // Which entities came and went since the scene was last loaded or patched: the document's against the ones this viewport knows. An entity the game
    // made or destroyed by itself (a script spawning a coin) is neither: it was never the document's, so the patch leaves it alone.
    std::vector<EntityGuid> added, removed;
    for (const auto& entry : in_source) {
        if (impl_->known.find(entry.first) == impl_->known.end()) added.push_back(entry.first);
    }
    for (const EntityGuid& guid : impl_->known) {
        if (in_source.find(guid) == in_source.end()) removed.push_back(guid);
    }
    // Everything is checked before anything is changed, so a refusal leaves the running scene as it was.
    for (const EntityGuid& guid : added) {
        if (!AllSpawnable(source, in_source.at(guid))) return false;
    }
    for (const EntityGuid& guid : removed) {
        const auto it = in_running.find(guid);
        if (it != in_running.end() && !AllSpawnable(running, it->second)) return false;
    }
    const std::unordered_set<EntityGuid, GuidHash> added_set(added.begin(), added.end()), removed_set(removed.begin(), removed.end());
    enum class Op { Copy, Add, Remove };
    struct Change {
        Entity target;
        ComponentId component;
        Op op;
        nlohmann::json value;
    };
    std::vector<Change> changes;
    for (const ComponentEdit& edit : edits) {
        if (edit.component == GetComponentId<Transform>() && !added_set.count(edit.entity) && !removed_set.count(edit.entity)) continue; // every Transform is copied below
        if (added_set.count(edit.entity) || removed_set.count(edit.entity)) continue; // an entity that came or went is handled whole
        if (!IsPatchable(edit.component)) return false;
        const auto from = in_source.find(edit.entity);
        const auto to = in_running.find(edit.entity);
        if (from == in_source.end()) return false; // the document no longer has it, yet it was not seen to go: something this cannot place
        if (to == in_running.end()) continue;      // the game destroyed it itself: nothing to edit
        const ComponentInfo& info = GetComponentInfo(edit.component);
        if (info.reflected == nullptr) return false;
        // A new parent moves the entity in the world, and a physics body or a script was built from where it stood when the scene loaded: those load again.
        if (edit.component == GetComponentId<Parent>() && !AllSpawnable(running, to->second)) return false;
        const bool there = source.HasComponentRaw(from->second, edit.component), here = running.HasComponentRaw(to->second, edit.component);
        if (there) changes.push_back({to->second, edit.component, here ? Op::Copy : Op::Add, reflect::ToJson(*info.reflected, source.GetComponentRaw(from->second, edit.component))});
        else if (here) changes.push_back({to->second, edit.component, Op::Remove, {}});
    }
    std::unordered_map<EntityGuid, Transform, GuidHash> wanted;
    source.ForEach<IdComponent, Transform>([&](const IdComponent& id, const Transform& transform) {
        if (!id.guid.IsNull()) wanted[id.guid] = transform;
    });
    // Apply: entities that came, entities that went, then the components, then every Transform.
    for (const EntityGuid& guid : added) {
        const Entity made = EntitySnapshot::Capture(source, in_source.at(guid)).Restore(running);
        if (made.IsNull()) continue;
        impl_->game->Guids().Add(guid, made);
    }
    for (const EntityGuid& guid : removed) {
        const auto it = in_running.find(guid);
        if (it == in_running.end() || !running.IsAlive(it->second)) continue;
        if (running.HasComponent<ScriptComponent>(it->second)) { // so its script ends properly (OnDestroy), which the lifecycle does only for a component taken off a living entity
            running.RemoveComponent<ScriptComponent>(it->second);
            impl_->game->GetLifecycle().Sync();
            if (!running.IsAlive(it->second)) continue;
        }
        impl_->game->Guids().Remove(guid);
        running.DestroyEntity(it->second);
    }
    for (const Change& change : changes) {
        if (!running.IsAlive(change.target)) continue;
        const ComponentInfo& info = GetComponentInfo(change.component);
        if (change.op == Op::Remove) {
            running.RemoveComponentRaw(change.target, change.component);
            continue;
        }
        // A script reads its settings once, when it is made: an edit takes the component off, lets the lifecycle end that instance (OnDisable, OnDestroy), and
        // puts it back with the new settings, which the next tick starts again. Only that entity's script restarts.
        if (change.op == Op::Copy && change.component == GetComponentId<ScriptComponent>()) {
            running.RemoveComponentRaw(change.target, change.component);
            impl_->game->GetLifecycle().Sync();
            if (!running.IsAlive(change.target)) continue;
            running.AddComponentRaw(change.target, change.component);
            ++impl_->script_restarts;
        }
        if (change.op == Op::Add) running.AddComponentRaw(change.target, change.component);
        reflect::FromJson(*info.reflected, running.GetComponentRaw(change.target, change.component), change.value);
    }
    // Only the Transforms the document changed since the last sync: a body the physics scene is moving (with Run game on) keeps moving, instead of being put back
    // where the document has it every time something else is edited.
    const auto same = [](const Transform& a, const Transform& b) {
        return a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z && a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y &&
               a.rotation.z == b.rotation.z && a.rotation.w == b.rotation.w && a.scale.x == b.scale.x && a.scale.y == b.scale.y && a.scale.z == b.scale.z;
    };
    running.ForEach<IdComponent, Transform>([&](IdComponent& id, Transform& transform) {
        if (id.guid.IsNull()) return;
        const auto it = wanted.find(id.guid);
        if (it == wanted.end()) return;
        const auto before = impl_->synced.find(id.guid);
        if (before == impl_->synced.end() || !same(before->second, it->second)) transform = it->second;
    });
    impl_->synced = wanted;
    impl_->known.clear();
    for (const auto& entry : in_source) impl_->known.insert(entry.first);
    ++impl_->patches;
    return true;
}

bool LiveViewport::Load(const std::filesystem::path& cooked_dir, const std::string& scene, u32 width, u32 height, std::string* error,
                        const std::vector<u8>* scene_bytes) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        Unload();
        return false;
    };
    Unload();
    width = std::clamp<u32>(width, 64, 3840);
    height = std::clamp<u32>(height, 64, 2160);
    auto impl = std::make_unique<Impl>();
    std::string problem;
    const std::vector<std::string> paks = player::GamePackage::FindPaks(cooked_dir); // each archive in the folder, as the player does
    if (paks.empty()) return fail("There is no .apak in " + cooked_dir.string() + ".");
    for (usize i = 0; i < paks.size(); ++i) {
        if (!impl->package.Mount(paks[i], static_cast<int>(i), &problem)) return fail(problem);
    }
    if (!impl->package.LoadManifest(&problem)) return fail(problem);
    impl->scene = scene;
    if (scene_bytes) impl->package.SetContentOverride(scene, *scene_bytes);
    impl->game = std::make_unique<player::Game>(impl->package);
    if (!impl->game->LoadScene(scene, &problem)) return fail("The scene \"" + scene + "\" did not load: " + problem);
    impl->swap_chain = device_.CreateSwapChain(nullptr, width, height, 2); // no window: an offscreen picture
    if (!impl->swap_chain) return fail("The device could not make an offscreen render target.");
    const u32 frame_count = std::max(impl->swap_chain->BufferCount(), 1u);
    for (u32 i = 0; i < frame_count; ++i) impl->commands.push_back(device_.CreateCommandList());
    impl->fences.assign(frame_count, 0);
    impl->renderer = std::make_unique<player::SceneRenderer>(device_, *impl->swap_chain, impl->package);
    impl_ = std::move(impl);
    RememberKnown();
    return true;
}

bool LiveViewport::Frame(f32 dt, const Vec3& position, const Quaternion& rotation, f32 fov_degrees) {
    if (!impl_) return false;
    Impl& live = *impl_;
    live.game->Tick(std::clamp(dt, 0.0f, 0.1f));
    bool placed = false;
    live.game->GetWorld().ForEach<Transform, Camera>([&](Transform& transform, Camera& camera) { // the scene's camera is the editor's
        if (placed) return;
        placed = true;
        transform.position = position;
        transform.rotation = rotation;
        if (fov_degrees > 0.0f) camera.fov_degrees = fov_degrees;
    });
    const usize slot = static_cast<usize>(live.frames % live.commands.size());
    device_.WaitForFence(live.fences[slot]);
    live.swap_chain->AcquireNextImage();
    rhi::ICommandList& cmd = *live.commands[slot];
    cmd.Reset();
    live.renderer->Draw(*live.game, cmd, static_cast<u32>(slot));
    cmd.Close();
    live.fences[slot] = device_.Submit(cmd, live.swap_chain.get());
    device_.WaitForFence(live.fences[slot]); // the picture is wanted now
    const bool read = live.swap_chain->ReadBack(pixels_);
    live.swap_chain->Present(false);
    ++live.frames;
    return read;
}

} // namespace aether::editor
