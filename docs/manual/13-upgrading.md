# Upgrading

What to check when you move a project or a plugin to a newer engine. The newest changes are listed in
[`docs/releases/NEXT.md`](../releases/NEXT.md).

## Before you upgrade

1. Commit your project. Saved files are versioned, and a newer engine can write a version an older one refuses,
   so keep a copy you can go back to.
2. Read the [API stability](api_stability.md) page: selected headers are **stable** and deprecated for a release
   before removal; other public APIs are **experimental** and can change in any release. This source-level policy
   does not promise binary ABI compatibility.
3. Build with warnings on. Deprecated calls warn with the version they were deprecated in and what replaces them.
4. Check [`format_versions.txt`](../design/format_versions.txt) before opening assets written by another engine
   version. It lists which older files migrate and which version mismatches are refused.

## Things that behave differently now

| Area | What to check |
| --- | --- |
| Load and save calls | Results of scene, prefab, glTF, image, plugin, project, input and pak load/save are `[[nodiscard]]`. Code that ignored them now warns (an error with warnings as errors). Check the result. |
| Saved files | Every loader refuses a file written by a newer engine and fails on wrong-typed data instead of throwing. The JSON scene loader accepts exactly its own version. See `docs/design/format_versions.txt`. |
| Format migrations | Binary scenes migrate version 1 to 2; animation clips and imported meshes migrate version 1 to 2. Other older versions are accepted only where the format inventory says so; do not assume opening a project upgrades every asset. |
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
- `LocalPluginRegistry` stores validated `.apkg` files by plugin name and numeric dotted version. Published versions
  are immutable; select the exact version to install. See the [registry and marketplace format](14-plugin-marketplace.md).

## After you upgrade

Run your project's checks and review load/save results. Save an asset in the new format only when you intend to stop
using it with an older engine; keep the backup made before upgrading. Opening a project does not rewrite every asset.

For Windows release packages, run `sha256sum -c SHA256SUMS.txt` in the directory containing the archives and manifest.
To verify the GitHub build attestation for an archive, use
`gh attestation verify AetherEditor-windows-x64.zip --repo rithymeth/game-ngine` (replace the filename for the game
archive). Authenticode is present only when the release manifest says `signed: true`; Sigstore provenance is published
independently of that setting.
