# Phase 48 — Production gate closure (2026-10-23)

Status: planned; evidence required. Base: master `830a155a683c2b46fbe1a643447a1bd843e364ea`.

## Objective
Turn existing engine, editor, Android/iOS and packaging checks into an evidence-backed Aether 1.0 release decision. Do not mark a gate passed based solely on implementation or commit messages.

## Confirmed baseline from repository history
- Android and iOS simulator smoke tests run the shared Lantern Run frozen-API scenario.
- Mobile build/run jobs and benchmark comparison have been promoted to required CI jobs in recent commits.
- Android Vulkan SurfaceView rendering, shader triangle, touch-input screenshot checks, and lifecycle suspend/resume tests have been added.
- Release zip checksums and build provenance attestations were added; Authenticode signing and support policy remain unresolved.
- Real-device mobile coverage, complete AETHER-01 content workflows, and target-PC performance evidence are not established by these commits.

## Work ledger
| ID | Owner role | Work | Acceptance evidence | Status |
| --- | --- | --- | --- | --- |
| P48-01 | DevOps | Verify required checks and branch protection on master | Exact CI run links and failing-check behavior | queued |
| P48-02 | Test | Verify AETHER-01 scene/component round-trip, older-project compatibility, source-free packaged launch | Reproducible commands, fixtures, diff reports | queued |
| P48-03 | Performance | Benchmark agreed minimum/recommended Windows D3D12 hardware | Named hardware, CPU/GPU frame-time percentiles, memory peaks | queued |
| P48-04 | Rendering | Validate skeletal animation, lighting/shadows, VFX and input in one authored vertical slice | Screenshots, logs, packaged game evidence | queued |
| P48-05 | Security | Audit plugin mounts, package verification, untrusted inputs and release artifacts | Test results and risk findings | queued |
| P48-06 | Release | Decide signing/support policy and verify clean-machine install | Signed-off policy and independent verification | blocked: product decision |
| P48-07 | Reviewer | Independent review of implementation diffs and test evidence | No unresolved P0/P1/P2 findings | queued |

## Definition of done
1. Each mandatory gate points to a specific run, artifact and commit.
2. Required CI failures block integration/release.
3. Windows x64 D3D12 AETHER-01 proof is verified end-to-end on a clean workstation.
4. Benchmarks have reproducible hardware and scene specifications, with documented thresholds.
5. Security and release risks are resolved or explicitly accepted by the owner.
6. A separate reviewer verifies the implementation; no evidence is invented.

## Scope boundary
Windows x64 D3D12 is the documented AETHER-01 shipping proof target. Mobile simulator tests are validation coverage, not certification of shipping support. Multiplayer 2.0, advanced rendering and other M8 pillars remain separate feature work unless required by a failing proof case.

## Handoff
Next: inspect CI workflows and gate evidence files, implement the smallest missing automated check, run it, and submit a reviewed pull request. This document is a coordination artifact, not proof that Phase 48 implementation or validation is complete.

## Phase 48 implementation increment — benchmark baseline integrity
- Added `tools/validate_benchmark_baseline.py`: rejects missing/duplicate benchmark names, non-finite or negative timings, and inconsistent timing ordering.
- CI Linux GCC now executes the validator immediately before its required benchmark regression comparison.
- This is a scoped preventive check, **not** evidence that benchmark performance or hardware targets pass.
- Validation still pending: run the script locally and inspect the branch CI result; independent review has not occurred.
