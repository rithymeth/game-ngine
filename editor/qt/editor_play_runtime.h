#pragma once

#include "aether/input/actions.h"
#include "aether/input/bindings.h"
#include "aether/project/project.h"

#include <filesystem>
#include <memory>

namespace aether {
class CharacterSystem;
class GuidIndex;
class JobSystem;
class Lifecycle;
class PhysicsScene;
class PhysicsWorld;
class World;
namespace assets { class AssetDatabase; }
namespace script {
class LuauHost;
class ScriptSystem;
}
}

namespace aether::editor::qt {

// Scene-scoped runtime used only while the Qt editor is in Play mode.
class EditorPlayRuntime final {
public:
    EditorPlayRuntime(World& world, GuidIndex& guids, const std::filesystem::path& content_root,
                      const ProjectSettings& project);
    ~EditorPlayRuntime();

    EditorPlayRuntime(const EditorPlayRuntime&) = delete;
    EditorPlayRuntime& operator=(const EditorPlayRuntime&) = delete;

    void Tick(f32 dt);
    void SetButton(input::Key key, bool down);
    void AddMouseDelta(f32 dx, f32 dy);
    void AddMouseWheel(f32 delta);
    void ClearInput();
    void Stop();

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
#if defined(AETHER_QT_EDITOR_SCRIPTING)
    std::unique_ptr<assets::AssetDatabase> assets_;
    std::unique_ptr<script::LuauHost> host_;
    std::unique_ptr<script::ScriptSystem> scripts_;
#endif
};

} // namespace aether::editor::qt
