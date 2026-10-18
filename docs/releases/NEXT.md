# Aether Engine — unreleased changes since v0.27.6

A working draft of the next release's notes. It is rewritten into `vX.Y.Z.md` when a version is tagged; it
describes what is on `master`, not a promise of a version number or a date.

## Agent and MCP tools

- The MCP server has tools for UI layouts, themes and animations, saves and settings, world partitions,
  materials, prefabs, projects and templates, and for a running game (log, UI and entities). A new sample game,
  Stardust Hop, was built entirely through them.
- Destructive tools (`destroy_entity`, `remove_component`, `save_scene`, `load_scene`) ask first: the first call
  returns a confirmation token, and a second call with `confirm_token` runs it. A project can set allow / confirm /
  deny per tool in `<scene folder>/.aether/agent_permissions.json`. Every call is logged, with arguments hashed, to
  `.aether/agent_session.jsonl` (8 MiB cap). Scene tools stay inside the scene folder (`--scene-root`).

## Gameplay systems

- AI decisions: utility brains, state trees and a GOAP planner, with JSON save and load.
- Navigation: a hierarchical path finder over tile borders, and 1000-agent crowd coverage. `NavMesh::FindPath`
  now reuses the path buffers you pass, so a reused `NavPath` does not allocate.
- Player: data-driven UI, script UI tables, scene lights, live quality presets, touch and pointer input, on-screen
  controls and platform lifecycle events.

## Networking

- Replication gains `SetRelevancy`, `SetPriority` and a shared `ReplicationConfig::bytes_per_second` cap.
- RPCs from a client are rate limited per client (`RpcConfig::calls_per_second`, active once `RpcRouter::Tick` is
  called each frame).
- The replication profile table is bounded (`ReplicationConfig::max_profiled_entities`); costs of despawned
  entities are folded into `NetProfile::retired_*`.

## World streaming

- `WorldStreamer` can read cells through an executor (a job system, say) with in-flight, entity and time budgets;
  only `Update()` touches the world.

## Assets and rendering

- ASTC texture cooking and GPU compressed textures, block-compressed textures on Vulkan, `DestroyTexture`, and
  memory-pressure trimming for mobile.
- Cooked meshes (`.amesh`) carry LODs and meshlets.

## Plugins

- Plugins can be packed into one `.apkg`, inspected without unpacking, checked for unmet dependencies, and
  installed with a staged, all-or-nothing install.
- Plugin descriptors carry an ABI number; a plugin built for another ABI is refused with a clear message.
- An offline local plugin registry stores immutable package versions and installs an exact selected version through the staged installer.

## Robustness and quality gates

- Every loader reads `$type` safely, rejects files from a newer engine and fails instead of throwing on
  wrong-typed data. Fuzz targets cover the pak reader, scenes, Blueprints, saves, input, UI, sprite data, plugins,
  the project file and crash reports, the AI data, and more.
- `aether_alloc_audit` (hot paths must not allocate) and `aether_soak` (heap growth over churn, streaming and
  replication) are CTest checks. The soak found and fixed an unbounded replication profile table.
- Load and save results on scenes, prefabs, glTF, images, plugins, projects, input and pak are `[[nodiscard]]`.

## Known gaps

- The hierarchical path finder is slower than plain Detour on the open meshes this was tried on; its benefit on
  very large meshes is unproven.
- A head-on crowd swap through a narrow middle jams.
- Cell decode time was measured only in a Debug build.
- The Qt editor builds only on Windows; most of the above is verified by the engine's tests, not in the editor.
