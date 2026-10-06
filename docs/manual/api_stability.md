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

`tools/check_api_stability.py` (also a test in the suite) checks that every module with public headers has a
level, and that every override names a real one. `--table` prints how many headers each module has at each level.

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
