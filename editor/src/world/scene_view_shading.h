#pragma once

#include "ui/image_texture.h"
#include "world/scene_view.h"

#include <string>
#include <vector>

namespace aether::editor {

class SceneDocument;

// How the Scene View shows the scene: its own wireframe of boxes over the grid, or a picture rendered by the real renderer (the Live
// Viewport's offscreen path) in one of its looks. Unlit and Normals are debug looks the scene shader can draw without relighting anything.
enum class SceneViewMode { Wireframe, Lit, Unlit, Normals };

// Which overlays the Scene View draws, in either mode: the ground grid, the transform gizmo, entity boxes, helpers (names, the selected
// entity's axes, the status text) and collision shapes.
struct SceneViewShowFlags {
    bool grid = true;
    bool gizmos = true;
    bool bounds = true;
    bool helpers = true;
    bool collision = false;
};

// The rendered picture behind a shaded Scene View. Implemented where a graphics device exists (editor/src/host/scene_shading.h); the
// portable panel only asks for a picture and a note, so it builds and runs headless, without Vulkan.
class SceneViewShading {
public:
    virtual ~SceneViewShading() = default;
    // True once a picture has been rendered at least once for the current scene.
    virtual bool Ready() const = 0;
    // Why there is no picture yet, or what it is doing: shown centred in the view.
    virtual const std::string& Note() const = 0;
    // Prepares the picture (cooking and loading the open scene the first time), keeps it up with the document's edits, and runs one frame
    // for `camera` at `width` x `height` pixels no more often than its own rate. `mode` is Lit, Unlit or Normals. True when the picture is
    // current (or was refreshed this call).
    virtual bool Advance(SceneDocument& document, const SceneViewCamera& camera, u32 width, u32 height, SceneViewMode mode, f32 dt) = 0;
    virtual const ImageTexture& Picture() const = 0;
    // What the newest picture's own frame says, for the view's stats row (the concept's "GPU 2.4 ms"): the GPU's time on the frame and how
    // many draws it recorded. `valid` is false where the picture has no timing (a device without GPU timers); zero draws means unknown.
    struct FrameStats {
        bool valid = false;
        f32 gpu_ms = 0.0f;
        u64 draw_calls = 0;
        u64 physics_bodies = 0; // the running game's bodies; zero means unknown
        u64 scripts = 0;        // its live script instances; zero means unknown
    };
    virtual FrameStats Stats() const { return {}; }
    // The running game's runtime errors (script callbacks and Blueprints). `entity_guid` is the target's EntityGuid text
    // -- empty when the error is not about one entity -- so the view can select it and a host can post a Messages link.
    struct RuntimeError {
        std::string text;
        std::string callback;
        std::string entity_guid;
        std::string entity_name;
    };
    virtual std::vector<RuntimeError> Errors() const { return {}; }
    // Stops the running scene and frees its picture (the view went back to the wireframe).
    virtual void Stop() = 0;
};

} // namespace aether::editor
