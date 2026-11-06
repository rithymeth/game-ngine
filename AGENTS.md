# Working on Aether

Conventions and traps for anyone — human or agent — changing this repository. Everything here was verified against the
code, most of it by getting it wrong first.

## Build and check

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
ctest --test-dir build -j 4
```

**CTest is the gate, not the test binary.** `ctest` runs 42 checks: the engine test suite, the governance gates (API
stability, API freeze, API inventory, format tables, docs index, MCP tool names) and the module-graph cycle check. Run
the whole thing before believing a change is fine.

**`aether_tests` is not idempotent.** Stale state under `E:/aether-test-temp` or `%TEMP%/aether_*` can make a run fail
before any test logic executes. Clear both before debugging a red suite — otherwise you will spend turns reverting a
change that was never the problem.

**`AETHER_TEST_FILTER` is a plain substring, not a regex.** And `AETHER_CHECK` does **not** abort, so a check followed by
an index or a dereference traps the process instead of reporting a failure. Guard before indexing.

## Traps that cost real time

- **`CreateEntity(..., IdComponent{})` gives a null GUID**, which `GuidIndex` skips. An entity looked up by GUID then
  silently isn't found. Call `EnsureAllGuids(world, guids)` first — it is the documented call.
- **`World::DestroyEntity` removes one entity and does not recurse.** Destroying an instance root leaves its subtree
  behind; the report from `InstantiatePrefab` names what to destroy (`ResolveReport::created_entities`).
- **PowerShell `Get-ChildItem -Include` matches nothing on a bare path.** Use `-Filter`. Likewise `Set-Content
  -Encoding utf8NoBOM` is not valid; use `[System.IO.File]::WriteAllLines`.
- **Here-strings use LF; most source files here are CRLF.** A `-replace` built from a here-string silently misses. Prefer
  the editor tools, or `[System.IO.File]::ReadAllText` with explicit contents.
- **`git add -u` does not stage new files.** `git add -A` in this repo picks up 8 000+ build artifacts; add paths
  explicitly.

## Where things live

The repository is **not** one source tree. `engine/` is only the core; `renderer/`, `net/`, `gameplay/`, `mcp/`, `editor/`,
`streaming/`, `save/`, `physics/`, `nav/`, `ai/` and more are siblings. **Search the whole repository** — several of the
worst mistakes in this repository's history were a grep scoped to one directory that missed the real implementation one
tree over.

Some facts that are easy to get wrong:

- `Parent` lives in `scene/hierarchy.h`, and **parenting is by GUID**, not by `Entity`.
- `AETHER_REFLECT` lives in `reflection/reflect_macros.h`, not `registry.h`.
- `EntityGuid::ToString` is a **free function**, not a member.
- `net` must never include `gameplay`; they are sibling targets and the build enforces it.
- Adding a component to `mcp/modules.cpp`'s `RegisterBuiltinComponents` is what makes it addable through the MCP.

## When changing the MCP

386 tools are registered across `mcp/*.cpp`. Two ctest checks guard them: **no tool name may be registered twice**, and
**every registered tool must be named in `mcp/README.md`** (and vice versa). Add the row.

Schemas declare `minimum`/`maximum`/`maxItems`/`maxLength` and the server now enforces them; tools still check their own
bounds. Do both.

## Performance claims

`docs/design/PERFORMANCE.md` holds the measured ones. **Measure before building**; several features here were built on an
assumption that a benchmark then contradicted, and the negative results are kept because they are worth more than the
feature was.