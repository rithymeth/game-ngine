#pragma once

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/gfx/rhi/swap_chain.h"
#include "aether/ecs/world.h"
#include "aether/scene/entity_guid.h"
#include "aether/math/quaternion.h"
#include "aether/math/vec.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace aether::editor {

namespace rhi = gfx::rhi;

// A running scene drawn by the real renderer inside the editor's own process: the cooked project is mounted, its scene loaded into a
// `Game` and drawn by the player's `SceneRenderer` into an offscreen swap chain on the editor's device, and each frame is read back as RGBA.
// So it is live in the sense that the game runs (physics, scripts, water, wind, animation) and the camera can be moved freely, but it
// shows the project as it was last cooked: an edit appears after the scene is saved and the viewport is loaded again. It does not
// share the editor's in-memory scene (the renderer draws a cooked package through a running Game; that is the part a rework would change).
class LiveViewport {
public:
    // `device` must outlive this, and be a Vulkan device.
    explicit LiveViewport(rhi::IDevice& device);
    ~LiveViewport(); // waits for the GPU

    LiveViewport(const LiveViewport&) = delete;
    LiveViewport& operator=(const LiveViewport&) = delete;

    // Mounts the cooked folder and starts `scene` (a path relative to the project's Content folder) at `width` x `height`. Replaces any scene
    // already loaded. With `scene_bytes` (a scene as SaveSceneToMemory writes it) the game runs that instead of the file the archives hold under
    // that path, so the scene need not be saved or cooked. False, with `error`, if it can't.
    bool Load(const std::filesystem::path& cooked_dir, const std::string& scene, u32 width, u32 height, std::string* error = nullptr,
              const std::vector<u8>* scene_bytes = nullptr);
    // Replaces the running scene with `scene_bytes` (the game loads it afresh: the world, physics and scripts start over). Models and
    // textures still come from the cooked archives, so an entity that names one that was not cooked is not drawn. False, with `error`, if the game rejects it.
    bool SetScene(const std::vector<u8>& scene_bytes, std::string* error = nullptr);
    // One value the document set inside a component an entity already has.
    struct ComponentEdit {
        EntityGuid entity;
        ComponentId component = 0;
    };
    // Brings the running scene to `source` (the open document's world) without reloading it, for a change that was only to values: every Transform, entity
    // by entity matched by GUID, and each component in `edits` that is one the renderer only reads (lights, the sky, wind, post-process volumes, decals),
    // copied across as the editor saved it. The game keeps its own state (physics, scripts). False, having changed nothing, when `source` and the
    // running scene do not hold the same identified entities with a Transform, or an edit names a component the running scene lacks or one that is not
    // safe to copy over a running game (a rigid body's mass is a physics body already built), so the caller falls back to SetScene. Counted in PatchCount.
    bool PatchScene(const World& source, const std::vector<ComponentEdit>& edits);
    bool PatchTransforms(const World& source) { return PatchScene(source, {}); }
    // Whether a component of that type can be patched into a running scene (PatchScene), for the caller that wants to know before it tries.
    static bool IsPatchable(ComponentId component);
    u64 PatchCount() const;
    // How many times SetScene has succeeded since Load.
    u64 SceneReloads() const;
    void Unload();
    bool Loaded() const { return impl_ != nullptr; }
    u32 Width() const;
    u32 Height() const;

    // Advances the game by `dt` seconds, puts the scene's camera at `position` looking as `rotation` (with `fov_degrees` if above zero),
    // draws, and reads the picture back. False if nothing is loaded or the picture could not be read.
    bool Frame(f32 dt, const Vec3& position, const Quaternion& rotation, f32 fov_degrees = 0.0f);
    // The last picture: Width() * Height() * 4 bytes, top row first.
    const std::vector<u8>& Pixels() const { return pixels_; }
    // How many frames have been drawn since Load.
    u64 FrameCount() const;
    // How many entities the loaded scene has.
    usize EntityCount() const;

private:
    struct Impl;
    rhi::IDevice& device_;
    std::unique_ptr<Impl> impl_;
    std::vector<u8> pixels_;
};

} // namespace aether::editor
