# ADR: replace Dear ImGui as the Aether Editor UI framework

- **Status:** Proposed; validate with a focused spike before committing to the migration.
- **Date:** 2026-10-07
- **Scope:** Editor presentation and host only. Aether's engine RHI and runtime remain independent.

## Context

The editor UX direction now calls for a persistent application shell, dockable
workspaces, saved layouts, accessible controls, searchable commands, asset
editors, and a project-to-ship workflow. Aether currently uses Dear ImGui for
both the Windows editor and the portable editor shell. Tool panels call ImGui
directly, the shell draws those panels through ImGui's Vulkan backend, and the
Windows host uses the D3D12 backend. This makes the UI toolkit part of the
presentation and host architecture, rather than a small skin that can be
swapped.

Dear ImGui remains useful for debug overlays and in-game developer tools.
The question here is whether it should remain the primary desktop editor UI.

## Recommendation

Use **Qt 6 Widgets** for the editor shell and desktop panels, subject to the
spike and licensing review below. Keep Aether's renderer and RHI as the source
of truth for viewport rendering. The editor UI toolkit must not choose,
replace, or become a runtime dependency of the engine renderer.

Qt Widgets is the best fit for Aether's C++ desktop IDE shape: it provides a
main-window and dock-widget model, tab/split docking, and layout
save/restore APIs. This maps directly to the editor's workspace presets and
persistent panels. It also avoids making every editor surface a custom-drawn
ImGui window.

Use Qt Widgets rather than Qt Quick for the first migration. Quick can host
custom graphics, but it introduces a scene-graph/render-loop integration
problem for the 3D viewport and is not needed to obtain dockable tool panels.
Reconsider Qt Quick only if the editor later needs animated, highly custom
surfaces that Widgets cannot deliver cleanly.

## Options considered

| Option | Advantages | Costs / risks | Assessment |
|---|---|---|---|
| **Qt 6 Widgets** | C++-native; mature menus, property forms, accessibility, docking, and saved main-window layouts; supports a custom central viewport host | New dependency and packaging/licensing review; Qt event loop, input, DPI, and native viewport integration must be proven; conversion of ImGui-coupled tools is substantial | **Recommended**, pending spike |
| **Dear ImGui docking branch** | Lowest migration cost; preserves existing immediate-mode panels and backend | Keeps ImGui as the editor's UI framework; custom accessibility/forms, document tabs, and polished native workflows remain Aether's burden | Good fallback if the spike fails; does not satisfy the technology change |
| **Qt Quick / QML** | Declarative styling and animations; Qt can render using Vulkan or Direct3D | More complex coordination between Qt's scene graph and Aether's own render loop/device; editor docking still needs extra work | Not the initial choice |
| **Embedded web UI (CEF/WebView)** | Familiar HTML/CSS workflow; broad visual flexibility | Bundled browser runtime, memory/startup cost, C++ bridge and editor/renderer surface complexity | Too much runtime weight for the first migration |

## Rendering and host boundary

Qt owns the editor window, menus, docks, focus, and input routing. Aether
continues to own viewport rendering, the render graph, and its selected RHI
backend. The viewport is hosted in a native child surface managed by the Qt
window. The host adapter translates Qt resize, focus, keyboard, mouse, and
DPI events into Aether editor input and supplies the native surface handle
needed by the existing RHI swap chain.

Do not start by embedding an independent Qt renderer into the Aether frame.
That would create two render loops and make synchronization, frame pacing,
and GPU-device ownership harder to reason about. Prove the viewport surface
can be created, resized, rendered, and destroyed safely with the existing
RHI before migrating panels.

Keep Dear ImGui available behind a narrow adapter for debug overlays and
during the transition. Do not expose ImGui types in the new shell, document,
command, or workspace interfaces. New editor features should use Qt widgets
and editor-owned presentation-neutral services.

## Validation spike and go/no-go gate

Build a small, disposable Qt Widgets host before migrating production tools.
It should include:

1. A main window with Hierarchy, Inspector, Content, and Console docks; tab,
   split, float, and close them; save and restore the layout and reset it.
2. A Ctrl+K command palette with keyboard-only filtering and invocation.
3. A native viewport child surface that creates the existing Aether RHI
   swap chain, resizes correctly, and renders a frame. Test Vulkan on Linux
   and Windows, and D3D12 on Windows.
4. Input/focus tests for gizmo drag, text fields, shortcuts, focus regain,
   high-DPI scale, minimize/restore, and viewport resize.
5. A packaging check for Qt modules and plugin deployment on supported
   developer machines and CI, plus review of Qt's applicable license terms.

**Go** only if the viewport uses the existing RHI without a second device or
frame loop, the same workspace model works in both editor hosts, and builds
are practical for the supported platforms. Otherwise, retain ImGui and use
the docking branch while documenting the failed gate.

## Migration sequence if the spike passes

1. Add Qt as an optional editor-only CMake dependency and build the empty host
   beside the current editors.
2. Define presentation-neutral editor services for commands, selection,
   documents, dirty state, and project context. Keep these services independent
   from both Qt and ImGui.
3. Replace the shell chrome first: menus, workspace switcher, command palette,
   docks, and status bar. Preserve existing tool access during transition.
4. Migrate a representative panel set (Content Browser, Hierarchy, Inspector,
   Console) and validate keyboard, accessibility, search, and persistence.
5. Port specialized editors in groups. Remove the production ImGui editor
   backends only when equivalent workflows pass on every supported platform.
6. Retain an opt-in ImGui debug overlay if engine development still benefits
   from it.

The editor remains an optional target. Qt must not be linked into runtime,
player, or game-project targets.

## Licensing and project policy

Qt has open-source and commercial licensing paths, with obligations that
vary by module and distribution model. The repository tree currently exposes
no root LICENSE file. Before adopting Qt, the project owner must establish
Aether's intended license and verify the selected Qt modules and deployment
model are compatible. This ADR is an engineering recommendation, not a legal
determination.

## References

- Qt Widgets main-window docking and layout persistence:
  [QMainWindow](https://doc.qt.io/qt-6/qmainwindow.html),
  [QDockWidget](https://doc.qt.io/qt-6/qdockwidget.html)
- Qt's Vulkan window/widget integration:
  [QVulkanWindow](https://doc.qt.io/qt-6/qvulkanwindow.html),
  [Hello Vulkan Widget example](https://doc.qt.io/qt-6/qtgui-hellovulkanwidget-example.html)
- Qt graphics API and licensing overview:
  [Qt Graphics](https://doc.qt.io/qt-6/topics-graphics.html),
  [Qt Licensing](https://doc.qt.io/qt-6/licensing.html)
