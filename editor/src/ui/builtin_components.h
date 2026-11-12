#pragma once

namespace aether::editor {

// Makes every built-in component type known to the reflection registry: the scene core, and each module's (renderer lights, Blueprint
// instances, animation, audio, navigation, sequencer, UI, particles, and the physics and kit components in a build that has them).
// A scene file is refused if it names a component the process does not know, so an editor that has not called this cannot open a scene
// that uses, say, a SkyLight or a Blueprint. Safe to call again. The MCP server keeps its own copy of the list (it cannot link this library).
void RegisterEditorComponents();

} // namespace aether::editor
