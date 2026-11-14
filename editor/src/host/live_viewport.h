#pragma once

#include "aether/player/graph_material.h"
#include "aether/player/scene_renderer.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/gfx/rhi/swap_chain.h"
#include "aether/ecs/world.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"
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
    // One component of an entity that stays, which the document set, added or removed.
    struct ComponentEdit {
        EntityGuid entity;
        ComponentId component = 0;
    };
    // Brings the running scene to `source` (the open document's world) without reloading it, for a change that did not rebuild the world: entities
    // the document created or destroyed since the viewport last synced (found by comparing the GUIDs it holds with the ones it last saw, so an
    // entity the game made by itself is left alone) are added or destroyed, each component in `edits` is set, added or removed as the document now has it,
    // and every Transform is copied, all matched by GUID. Only what the renderer reads each frame is done this way (lights, the sky, wind, post-process
    // volumes, decals, models, names, and the parent and prefab links that tie entities together); the game keeps its own state (physics, scripts). A script is made and
    // ended by the game's lifecycle as its component comes and goes; an edit to one restarts that script alone (OnDestroy, then OnCreate and OnStart again).
    // False, having changed nothing, when a change involves anything else (a character controller or an audio source is built into the running game when it
    // loads, so an entity that carries one, or an edit to one, needs SetScene). Counted in PatchCount.
    bool PatchScene(const World& source, const std::vector<ComponentEdit>& edits);
    bool PatchTransforms(const World& source) { return PatchScene(source, {}); }
    // Whether a component of that type can be patched into a running scene (PatchScene), for the caller that wants to know before it tries.
    static bool IsPatchable(ComponentId component);
    u64 PatchCount() const;
    // How many scripts a patch has restarted because their component was edited.
    u64 ScriptRestarts() const;
    // How many times SetScene has succeeded since Load.
    u64 SceneReloads() const;
    void Unload();
    bool Loaded() const { return impl_ != nullptr; }
    u32 Width() const;
    u32 Height() const;

    // Advances the game by `dt` seconds, puts the scene's camera at `position` looking as `rotation` (with `fov_degrees` if above zero),
    // draws, and reads the picture back. The scene keeps its own camera (the active one, or one made here when it has none); `projection`
    // and `ortho_height` select the parallel view. False if nothing is loaded or the picture could not be read.
    bool Frame(f32 dt, const Vec3& position, const Quaternion& rotation, f32 fov_degrees = 0.0f, Projection projection = Projection::Perspective,
               f32 ortho_height = 0.0f);
    // The look of the next frames: the scene pass's lit shading, or the editor's unlit and normals debug views.
    void SetViewMode(player::SceneRenderer::ViewMode mode);
    // The last picture: Width() * Height() * 4 bytes, top row first.
    const std::vector<u8>& Pixels() const { return pixels_; }
    // How many frames have been drawn since Load.
    u64 FrameCount() const;
    // How many entities the loaded scene has.
    usize EntityCount() const;
    // How many physics bodies the running game has (as of its last physics step: bodies follow the world's colliders and rigid bodies when the game steps).
    usize PhysicsBodyCount() const;
    // Reads a material graph (.amat) from the running game's package and checks it (player/graph_material.h): whether the game could draw with it.
    player::GraphMaterialReport CompileGraphMaterial(const std::string& path) const;
    // Serves `bytes` as the content file `path` from now on (a material graph the person has just saved) and has the game's renderer build every graph's
    // shader again, so the next frame shows it. False if nothing is loaded.
    bool SetGraphFile(const std::string& path, std::vector<u8> bytes);
    // How many script instances the running game has (one per entity with a working script).
    usize ScriptInstanceCount() const;
    // Where an entity is in the running game (its Transform's position, which a simulated body moves), false if the game has no such entity.
    bool PositionOf(const EntityGuid& entity, Vec3& position) const;

private:
    void RememberKnown(); // the identified entities of the running scene are what the document has
    struct Impl;
    rhi::IDevice& device_;
    std::unique_ptr<Impl> impl_;
    std::vector<u8> pixels_;
};

} // namespace aether::editor
