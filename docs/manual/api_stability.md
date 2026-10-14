# API stability

Aether marks how much you can rely on each public API. There are three levels:

| Level | What it means |
| --- | --- |
| **stable** | Covered by the deprecation policy below: it is not removed or changed incompatibly without a deprecation release first. |
| **experimental** | May change in any release. This is where everything starts, and where everything is until the API freeze for 1.0 promotes the parts that are ready. |
| **internal** | Not for use outside its module and its tests. |

Each public module has a level in [`docs/design/api_stability.txt`](../design/api_stability.txt). A single
header can differ from its module with a comment line near the top:

```cpp
// @stability: stable
```

## Current compatibility status

At engine version 0.27.6, all public modules are marked **experimental**; no
public C++ API currently carries a stability guarantee. The annotation checker
confirms that public modules and header overrides have valid labels. It does
not compare C++ declarations or binary ABI against a previous release. Those
compatibility checks and the decision to promote selected APIs belong to the
Core API 1.0 freeze; do not treat the labels alone as proof of compatibility.

`tools/check_api_stability.py` (also a test in the suite) checks that every module with public headers has a
level, and that every override names a real one. `--table` prints how many headers each module has at each level.
Stable headers are frozen: [`docs/design/api_frozen.txt`](../design/api_frozen.txt) records the names each one
declares, and the same check fails when one disappears. To retire a name, deprecate it for a release, then
remove it and list it in [`docs/design/api_removed.txt`](../design/api_removed.txt) (`<header> <name>`). New names
need nothing; `python3 tools/check_api_stability.py --update-frozen` adds them to the baseline.
A game built on the frozen API is checked too: `samples/frozen_api_game` (Lantern Run) is written the way a third party
would write one, and `python3 tools/check_api_stability.py --consumer <folder>` fails if any source in the folder
includes a header that is not `stable`. A stable header may include only stable or internal ones. The checks
themselves are tested by `tests/check_api_freeze.py` (CTest `api_freeze_checks`).
The [Core API inventory](core_api_inventory.md) records current ownership,
threading and failure behavior for the engine's central public headers.

## Deprecating something

`aether/core/api.h` has the macros. To retire an API, keep it for at least one release and mark it:

```cpp
#include "aether/core/api.h"

AETHER_DEPRECATED("0.40", "Spawner::SpawnAt") Entity SpawnPrefab(const Vec3& at);
```

Callers get a compiler warning that says since when it is deprecated and what replaces it. Code inside Aether
that still has to call it defines `AETHER_ALLOW_DEPRECATED` while it migrates. `AETHER_STABLE`,
`AETHER_EXPERIMENTAL` and `AETHER_INTERNAL` can label a single declaration the same way; they are
documentation and change nothing in the build.

## The policy

- A **stable** API is deprecated for at least one release before it is removed, and the deprecation names the replacement.
- A serialized format is never changed without a version and a migration (the existing rule for every saved type).
- **Experimental** changes are listed in the release notes but need no deprecation period.

## Results you must check

A function whose failure the caller has to handle returns a type or bool marked `[[nodiscard]]`, so ignoring the result is a compiler warning. `SaveResult` is marked, so every save, settings and envelope call is covered; so are `cook::LoadAtex`, `cook::ParseTextureFormat` and `cook::ParseConfiguration`. When ignoring one is really fine (a sample the editor writes for a tool), say so with `(void)` and a comment.
