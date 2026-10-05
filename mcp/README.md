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
| `play` / `pause` / `stop` | Play-in-Editor; stop restores the scene exactly |

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
- `main.cpp` -- the `aether_mcp_server` executable.
- Tests: `tests/test_mcp.cpp`.

Only components registered in the process are visible. The server registers
the built-ins (`Transform`, `Parent`, `IdComponent`, `ModelRenderer`); to expose
more (physics, audio, ...) call `GetComponentId<T>()` for them in `main.cpp`.
