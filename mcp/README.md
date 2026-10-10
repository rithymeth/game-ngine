# Aether MCP server

A [Model Context Protocol](https://modelcontextprotocol.io) server that lets an
AI agent (Claude Code, Claude Desktop, any MCP client) drive the Aether
editor's scene: inspect, create, edit and delete entities and components,
reparent, undo/redo, save and load scenes, and use Play-in-Editor.

Every change goes through the editor's undoable command stack
(`editor/src/core/commands.h`), so an agent's edits can be undone with the same
history a human uses. It is headless -- no window, graphics backend or ImGui --
and talks JSON-RPC over stdio.

## Use it

```
cmake --build <build-dir> --target aether_mcp_server
claude mcp add aether -- <build-dir>/mcp/aether_mcp_server [--scene scene.json]
```

stdout carries the protocol only; engine logging below Error is switched off
and errors go to stderr.

## Tools

| Tool | What it does |
| --- | --- |
| `editor_state` | Entity count, undo/redo labels, unsaved changes, play state |
| `list_component_types` | Component names and their fields |
| `list_entities` | GUIDs and component names; optional component filter |
| `get_entity` | Every component of one entity, as JSON |
| `create_entity` | New entity with optional components and parent (all-or-nothing) |
| `destroy_entity` | Delete an entity |
| `add_component` / `remove_component` | Add (with optional initial values) or remove a component |
| `set_component` | Partial update: only the fields named change |
| `reparent` | Parent or detach an entity; rejects cycles |
| `undo` / `redo` | Walk the editor history |
| `save_scene` / `load_scene` | `.json` (readable) or `.aesc` (binary); load can `replace` |
| `play` / `pause` / `stop` | Play-in-Editor; stop restores the scene exactly. `play` starts the simulation systems |
| `list_systems` | The simulation systems in execution order, with enabled state (play mode) |
| `set_system_enabled` | Turn one system on or off |
| `step_simulation` | Run N frames (1-10000) of `dt` seconds through the fixed-timestep frame loop |
| `set_platformer_input` | Move/jump input for 2D platformer controllers |

Entities are addressed by GUID (from `list_entities`), which survives
undo/redo and save/load. Component values use the engine's reflection JSON
(`Vec3` is `[x, y, z]`, entity references are GUID strings).
Bad input is reported as a tool error (`isError`), never a crash, and a failed
`create_entity` leaves the scene untouched.

## Layout

- `mcp_server.h/.cpp` -- the protocol: initialize, ping, `tools/list`,
  `tools/call`; transport-independent `HandleMessage`, plus the stdio loop.
- `editor_tools.h/.cpp` -- the tools above, over an `EditorSession` (world,
  GUID index, command stack, play session).
- `modules.cpp` -- registers every module's components (animation, audio, Blueprint, gameplay/GAS,
  navigation, renderer, sequencer, 2D, streaming, UI, VFX, and physics and the genre kits when built).
- `simulation.h/.cpp` -- the systems that run in play mode (3D physics and characters, 2D physics with
  platformers and camera follow, previous-transform recording) on the engine's scheduler.
- `main.cpp` -- the `aether_mcp_server` executable.
- Tests: `tests/test_mcp.cpp`.

Every module's components are registered at startup, so all of them can be listed, added and edited.
Gameplay AI (`Aether::AI`: behaviour trees, perception) is deliberately not linked: the editor/MCP side
must not depend on it (`docs/design/agent_gameplay_ai_boundary.md`).

Systems that need project assets or a window (audio output, particles, sequences, UI, scripting) are not
in the headless simulation; they need a full `Game`.
