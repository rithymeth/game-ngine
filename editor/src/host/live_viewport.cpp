#include "host/live_viewport.h"

#include "aether/core/log.h"
#include "aether/player/game.h"
#include "aether/player/scene_renderer.h"
#include "aether/scene/components.h"
#include "aether/reflection/serialize.h"
#include "aether/renderer/components.h"
#include "aether/scene/gameplay.h"

#include <algorithm>
#include <unordered_map>

namespace aether::editor {

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
usize LiveViewport::EntityCount() const { return impl_ && impl_->game ? impl_->game->GetWorld().EntityCount() : 0; }

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
    return true;
}

bool LiveViewport::IsPatchable(ComponentId component) {
    // Only what the renderer reads each frame and nothing else holds a copy of: a physics body, a script or an audio source is built from its component
    // when the scene loads, so editing one of those needs the scene loaded again.
    return component == GetComponentId<Transform>() || component == GetComponentId<DirectionalLight>() || component == GetComponentId<PointLight>() ||
           component == GetComponentId<SpotLight>() || component == GetComponentId<SkyLight>() || component == GetComponentId<Wind>() ||
           component == GetComponentId<WindZone>() || component == GetComponentId<PostProcessVolume>() || component == GetComponentId<Decal>();
}

bool LiveViewport::PatchScene(const World& source, const std::vector<ComponentEdit>& edits) {
    if (!impl_ || !impl_->game) return false;
    struct Hash {
        usize operator()(const EntityGuid& g) const noexcept { return static_cast<usize>(g.hi ^ (g.lo * 0x9e3779b97f4a7c15ull)); }
    };
    std::unordered_map<EntityGuid, Transform, Hash> wanted;
    source.ForEach<IdComponent, Transform>([&](const IdComponent& id, const Transform& transform) {
        if (!id.guid.IsNull()) wanted[id.guid] = transform;
    });
    World& running = impl_->game->GetWorld();
    // Every identified entity of the running scene must be in `source` and the other way round, or something was added or removed: that is a reload's job.
    usize found = 0;
    bool same = true;
    std::unordered_map<EntityGuid, Entity, Hash> running_entities;
    running.ForEach<IdComponent, Transform>([&](IdComponent& id, Transform&) {
        if (id.guid.IsNull()) return;
        if (wanted.find(id.guid) == wanted.end()) same = false;
        else ++found;
    });
    if (!same || found != wanted.size()) return false;
    // The edited components, checked before anything is changed: both worlds must have the entity and the component, and the component must be patchable.
    struct Copy {
        Entity target;
        const ComponentInfo* info;
        nlohmann::json value;
    };
    std::vector<Copy> copies;
    if (!edits.empty()) {
        std::unordered_map<EntityGuid, Entity, Hash> in_source;
        source.ForEachArchetype([&](const Archetype& archetype) {
            for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
                const Entity* array = archetype.EntityArray(chunk);
                for (u32 row = 0; row < archetype.ChunkEntityCount(chunk); ++row) {
                    if (const IdComponent* id = source.GetComponent<IdComponent>(array[row])) in_source[id->guid] = array[row];
                }
            }
        });
        running.ForEachArchetype([&](const Archetype& archetype) {
            for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
                const Entity* array = archetype.EntityArray(chunk);
                for (u32 row = 0; row < archetype.ChunkEntityCount(chunk); ++row) {
                    if (const IdComponent* id = running.GetComponent<IdComponent>(array[row])) running_entities[id->guid] = array[row];
                }
            }
        });
        for (const ComponentEdit& edit : edits) {
            if (edit.component == GetComponentId<Transform>()) continue; // every Transform is copied below
            if (!IsPatchable(edit.component)) return false;
            const auto from = in_source.find(edit.entity);
            const auto to = running_entities.find(edit.entity);
            if (from == in_source.end() || to == running_entities.end()) return false;
            const ComponentInfo& info = GetComponentInfo(edit.component);
            if (info.reflected == nullptr || !source.HasComponentRaw(from->second, edit.component) || !running.HasComponentRaw(to->second, edit.component)) return false;
            copies.push_back({to->second, &info, reflect::ToJson(*info.reflected, source.GetComponentRaw(from->second, edit.component))});
        }
    }
    for (const Copy& copy : copies) {
        const ComponentId id = FindComponentIdByName(copy.info->reflected->name);
        if (id == kInvalidComponentId || !reflect::FromJson(*copy.info->reflected, running.GetComponentRaw(copy.target, id), copy.value)) return false;
    }
    running.ForEach<IdComponent, Transform>([&](IdComponent& id, Transform& transform) {
        if (id.guid.IsNull()) return;
        transform = wanted[id.guid];
    });
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
