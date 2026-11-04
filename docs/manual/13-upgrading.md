# Upgrading

What to check when you move a project or a plugin to a newer engine. The newest changes are listed in
[`docs/releases/NEXT.md`](../releases/NEXT.md).

## Before you upgrade

1. Commit your project. Saved files are versioned, and a newer engine can write a version an older one refuses,
   so keep a copy you can go back to.
2. Read the [API stability](api_stability.md) page: **stable** APIs are deprecated for a release before they are
   removed; **experimental** ones can change in any release.
3. Build with warnings on. Deprecated calls warn with the version they were deprecated in and what replaces them.

## Things that behave differently now

| Area | What to check |
| --- | --- |
| Load and save calls | Results of scene, prefab, glTF, image, plugin, project, input and pak load/save are `[[nodiscard]]`. Code that ignored them now warns (an error with warnings as errors). Check the result. |
| Saved files | Every loader refuses a file written by a newer engine and fails on wrong-typed data instead of throwing. The JSON scene loader accepts exactly its own version. See `docs/design/format_versions.txt`. |
| MCP destructive tools | `destroy_entity`, `remove_component`, `save_scene` and `load_scene` return a confirmation token first; pass it back as `confirm_token`. Set `.aether/agent_permissions.json` to allow or deny a tool outright. |
| MCP scene paths | Scene tools stay inside the scene folder; pass `--scene-root` to use another. |
| RPC rate limit | Once `RpcRouter::Tick(now)` is called on the server, each client may make `calls_per_second` (default 120, burst 60) calls. Set it to 0 to turn it off. Without `Tick` nothing is limited. |
| Replication | Defaults are unchanged. `bytes_per_second`, `SetRelevancy` and `SetPriority` are opt-in. The profile keeps at most `max_profiled_entities` (4096) entities; read `NetProfile::retired_*` for the rest. |
| `NavMesh::FindPath` | It clears and refills the `NavPath` you pass instead of replacing it; a reused path keeps its capacity. |
| Streaming | `WorldStreamer` is synchronous unless you call `SetExecutor`. The loader you give it must be thread-safe then. |

## Plugins

- A plugin built for another `kPluginAbi` is refused when enabled; rebuild it. A descriptor without `abi` is
  accepted.
- To ship one, pack its folder with `PackagePlugin`, and install with `InstallPluginPackage` after checking
  `UnmetDependencies`. A package holds one folder named after the plugin and nothing outside it.

## After you upgrade

Run your tests, then open each project once and save it so its files are written in the current formats.
