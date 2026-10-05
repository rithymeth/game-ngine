#pragma once

// API stability annotations (Phase 37 step 3, docs/design/UPGRADE_PLAN.md, docs/manual/api_stability.md).
//
// Every public module has a stability level in docs/design/api_stability.txt, and a header can
// override it with a comment line `// @stability: stable|experimental|internal`.
//
//   stable        covered by the deprecation policy: no removal without a deprecation release first
//   experimental  may change in any release; the default until Phase 48 freezes the API
//   internal      not for use outside the module (and its tests)
//
// The macros below are documentation for readers and tools; they don't export or hide anything.

#define AETHER_STABLE
#define AETHER_EXPERIMENTAL
#define AETHER_INTERNAL

// Marks an API as scheduled for removal: AETHER_DEPRECATED("0.40", "UseNewThing") on a declaration
// says since when it is deprecated and what replaces it. It becomes a compiler deprecation warning, so
// callers see it; code inside Aether defines AETHER_ALLOW_DEPRECATED to silence it while it migrates.
#ifdef AETHER_ALLOW_DEPRECATED
#define AETHER_DEPRECATED(since, replacement)
#else
#define AETHER_DEPRECATED(since, replacement) [[deprecated("deprecated since " since "; use " replacement)]]
#endif
