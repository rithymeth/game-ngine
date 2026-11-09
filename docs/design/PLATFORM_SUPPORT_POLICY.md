# Platform support policy

What Aether supports today, what it only *validates* today, and what it does not support yet — with
what it would take to promote anything from one column to the next. This file is the one the
readiness matrix and the top-level README point at; it consolidates statements that were previously
only in a retired planning doc (phases 24, 44 and 45) and in `docs/design/AETHER_01_ENGINE_READINESS_MATRIX.md`.

## The rule

**A compile is not support.** A platform counts as supported when the engine builds there, the test
suite runs there, a packaged build of a sample runs there, and the graphics backend it uses has been
checked against the parity harness (`aether/gfx/parity.h`) — not when a build happens to link.

## Tiers

| Tier | Platform | What is verified |
|---|---|---|
| **Release target** | Windows x64, MSVC, D3D12 | Builds, full test suite, the packaged samples, and the D3D12 renderer is the one the editor and samples ship on. This is the platform a release is cut for. |
| **Engine validation** | Linux x86_64 | CI builds the engine and runs the test suite. The portable editor shell runs headless. No graphics parity lane is required on every change. |
| **Build/test validation** | macOS arm64, MoltenVK | CI builds and runs the test suite on Apple Silicon, including the editor's portable shell under test. MoltenVK is exercised, but this is not a shipped target. |
| **Shell only** | Android arm64, iOS arm64 | Phase 45 has started: a Gradle/JNI app shell and an unsigned Xcode app shell link the native core, and input/lifecycle pieces exist (`PlatformLifecycle` covers suspend, resume, memory pressure and permissions). **No device has run a gameplay build**, so neither is a target. |
| **Deferred** | Console, XR, web | Not started. No platform SDKs are integrated. |

## Promotion gates

Moving a platform up a tier needs, in order:

1. **Build** in CI on every change, so it cannot silently rot.
2. **Test suite** green on that platform — the same `ctest` set, with platform-specific skips only
   where a capability genuinely does not exist.
3. **A packaged sample run**, not just a compiled executable.
4. **Graphics parity** against the reference for anything that renders, using the parity harness
   rather than eyeballing a screenshot.
5. **Device coverage** for mobile: the device farm in a retired planning doc 45.7 is the mechanism, and it
   needs an account for a paid service, which is an **open decision** — until it is made, mobile stays
   in the "shell only" tier no matter how well the shell builds.

## Open decisions

- The device farm (45.7) — paid service, not yet chosen.
- Whether Linux is ever promoted past engine validation to a release target.
- Whether macOS/MoltenVK is promoted to a release target, which would require graphics parity lanes
  on Apple hardware.
