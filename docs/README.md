# Aether documentation

The offline documentation site is generated from this directory and the
engine reflection registry. Build `aether_docs_site` with CMake, or run
`aether_docgen --site build/docs-site --docs docs` from the repository root.
Open `build/docs-site/index.html` in a browser; the generated site works
without a server or network connection.

| Document | What it's for |
|---|---|
| [ROADMAP.md](ROADMAP.md) | The overall plan: Phases 6–48, including M7's Aether 1.0 production gate and the improvement-only M8 milestone |
| [design/UPGRADE_PLAN.md](design/UPGRADE_PLAN.md) | The PR-sized plan for M7, the Aether Upgrade (Phases 37–48) |
| [design/M8_PRODUCTION_TECHNOLOGY.md](design/M8_PRODUCTION_TECHNOLOGY.md) | Improvement-only M8: harden and tune existing engine systems and workflows without adding feature families |
| [design/ENGINE_FOUNDATION_PLAN.md](design/ENGINE_FOUNDATION_PLAN.md) | Capability-gap roadmap and evidence gates to make the engine ready for AETHER-01 |
| [design/AETHER_01_PROOF_PLAN.md](design/AETHER_01_PROOF_PLAN.md) | Windows proof-release plan for hardening existing engine workflows and building the AETHER-01 First Contact game slice |
| [design/M8_PRODUCTION_PLAN.md](design/M8_PRODUCTION_PLAN.md) | Proposed twelve-phase Aether 1.0 product gate, beyond the current M8 improvement scope |
| [design/m8_gates.json](design/m8_gates.json) | Evidence status for that product-gate proposal |
| [ROADMAP_DETAILS.md](ROADMAP_DETAILS.md) | File formats, the Phase 6 (reflection) build spec, Blueprint VM design, extra editor mockups, shortcuts, estimates, risks, PR checklist, glossary |
| [design/PHASE_SPECS.md](design/PHASE_SPECS.md) | Build specs for the critical-path phases: 7 (undo/redo, GUIDs, Play-in-Editor), 8 (asset database, hot reload), 9 (prefabs, scheduling), 11 (Luau), 13 (physics events, character movement) |
| [design/EDITOR_UI.md](design/EDITOR_UI.md) | Editor UI design system: color/type/spacing tokens, icons, layouts, widgets, panel specs, interaction rules, feedback, accessibility, and the plan for splitting editor/main.cpp |
| [design/AETHER_ENGINE_UX.md](design/AETHER_ENGINE_UX.md) | Complete product UX architecture: Project Hub, editor workspaces, shared panels/services, core workflows, accessibility, toolkit boundary, migration, and acceptance criteria |
| [design/EDITOR_UX_2.md](design/EDITOR_UX_2.md) | Detailed Editor 2.0 shell and workflow specification |
| [design/EDITOR_UI_TOOLKIT_ADR.md](design/EDITOR_UI_TOOLKIT_ADR.md) | Proposed UI framework change: Qt Widgets recommendation, RHI/viewport boundary, alternatives, licensing gate, and a go/no-go validation spike |
| [design/BLUEPRINT_NODES.md](design/BLUEPRINT_NODES.md) | Every node in the first Blueprint library with pins and behavior, compiler messages, and the node test plan |
| [manual/README.md](manual/README.md) | The manual: getting started, projects and assets, scripting, input, 2D, packaging, plugins and editor extensions, and the generated API reference |
| [manual/01-getting-started.md](manual/01-getting-started.md) | Windows path from a clean checkout to a playable sample game |
| [tutorials/FIRST_GAME.md](tutorials/FIRST_GAME.md) | "Coin Run": how making a game will work once M2 is done, used as the M2 acceptance test |
| [WINDOWS_REQUIREMENTS.md](WINDOWS_REQUIREMENTS.md) | What a Windows machine needs to run an Aether binary |
| [SDK.md](SDK.md) | Using the installed CMake SDK: module libraries, public headers and dependency libraries |

## Readiness and evidence

| Document | What it's for |
|---|---|
| [design/AETHER_01_ENGINE_READINESS_MATRIX.md](design/AETHER_01_ENGINE_READINESS_MATRIX.md) | Every AETHER-01 requirement with its evidence status and the gate that would prove it |
| [design/MOBILE_READINESS.md](design/MOBILE_READINESS.md) | What Phase 45 has built for Android and iOS, and the fact that neither has run on a device |
| [design/PLATFORM_SUPPORT_POLICY.md](design/PLATFORM_SUPPORT_POLICY.md) | Release, validation, shell-only and deferred tiers, the promotion gates and the open decisions |
| [design/CONSOLE_CERTIFICATION.md](design/CONSOLE_CERTIFICATION.md) | Console certification checklist: the engine-side release evidence that can be gathered |
| [design/RHI_RESOURCE_LIFETIME.md](design/RHI_RESOURCE_LIFETIME.md) | How GPU objects made through `IDevice` are owned, destroyed and retired, on Vulkan and D3D12 alike |
| [design/PERFORMANCE.md](design/PERFORMANCE.md) | The performance record: measured numbers, where each came from, and the experiments that did not work |
| [design/PHYSICS_CONSTRAINTS.md](design/PHYSICS_CONSTRAINTS.md) | The v1 physics constraint API (braced doors, containment) and its consumer |
| [design/large_world_coordinates.md](design/large_world_coordinates.md) | Floating-origin work, what is done, and the `Vec3d` gameplay decision still open |
| [design/agent_gameplay_ai_boundary.md](design/agent_gameplay_ai_boundary.md) | Which AI runs inside the game simulation and which runs outside it |
| [design/history_rewrite.md](design/history_rewrite.md) | Runbook for dropping the large binaries from history, with its status and where the backup is |
| [design/M8_PACKAGE_RELIABILITY.md](design/M8_PACKAGE_RELIABILITY.md) | The M8 increment over cook, package, verify and release |

## Editor migration

| Document | What it's for |
|---|---|
| [design/EDITOR_QT_UX_ROADMAP.md](design/EDITOR_QT_UX_ROADMAP.md) | The Qt editor UI/UX migration plan and its current step |
| [design/QT_EDITOR_FEATURE_COVERAGE.md](design/QT_EDITOR_FEATURE_COVERAGE.md) | Acceptance ledger for replacing the ImGui editor with Qt 6 |

## Game projects

| Document | What it's for |
|---|---|
| [design/GHOST_SHIFT_ENGINE_INTEGRATION.md](design/GHOST_SHIFT_ENGINE_INTEGRATION.md) | Ghost Shift's integration and upgrade plan against this engine |
| [design/GHOST_SHIFT_INVENTORY.md](design/GHOST_SHIFT_INVENTORY.md) | Ledger item GS-00: what the engine actually provides for Ghost Shift |

## Manual chapters

| Document | What it's for |
|---|---|
| [manual/02-projects-and-assets.md](manual/02-projects-and-assets.md) | Projects, the content tree, importing and the asset database |
| [manual/03-scripting.md](manual/03-scripting.md) | Luau and Blueprints together |
| [manual/04-input.md](manual/04-input.md) | Actions and mapping contexts, and rebinding |
| [manual/05-2d.md](manual/05-2d.md) | The `sprite2d` toolkit: sprites, tilemaps and the pixel-perfect camera |
| [manual/06-packaging.md](manual/06-packaging.md) | Cooking, packaging and shipping |
| [manual/07-extending.md](manual/07-extending.md) | Plugins and editor extensions |
| [manual/08-api-reference.md](manual/08-api-reference.md) | The generated reference for every reflected component, struct and enum |
| [manual/09-gameplay.md](manual/09-gameplay.md) | Gameplay tags, attributes, effects and abilities |
| [manual/10-inventory.md](manual/10-inventory.md) | The inventory kit: bags, stacks, weight, equipment and usable items |
| [manual/11-interaction.md](manual/11-interaction.md) | The interaction kit: doors, levers and anything a character uses |
| [manual/12-quests.md](manual/12-quests.md) | The quest kit and its objectives |
| [manual/13-upgrading.md](manual/13-upgrading.md) | What to check when moving a project or plugin to a newer engine |
| [manual/14-plugin-marketplace.md](manual/14-plugin-marketplace.md) | The plugin registry and marketplace format |
| [manual/15-dedicated-server.md](manual/15-dedicated-server.md) | `aether_server`: hosting a cooked game without a window or GPU |
| [manual/api_stability.md](manual/api_stability.md) | The three stability levels and what each promises |
| [manual/core_api_inventory.md](manual/core_api_inventory.md) | Ownership and threading inventory for the APIs a game or plugin uses |
| [manual/support_policy.md](manual/support_policy.md) | A support-policy proposal, explicitly not yet promised |

## Release notes

| Document | What it's for |
|---|---|
| [releases/NEXT.md](releases/NEXT.md) | The working draft of the next release; rewritten into `vX.Y.Z.md` when a version is tagged |
| [releases/v0.28.0.md](releases/v0.28.0.md) | v0.28.0: netcode and MCP hardening, level instances, and AGENTS.md |
| [releases/v0.27.6.md](releases/v0.27.6.md) | v0.27.6: Qt editor startup fix |
| [releases/v0.27.5.md](releases/v0.27.5.md) | v0.27.5: the current editor in the Windows package |
| [releases/v0.27.4.md](releases/v0.27.4.md) | v0.27.4: release and cook reliability |
| [releases/v0.27.3.md](releases/v0.27.3.md) | v0.27.3: editor data safety and readiness |
| [releases/v0.27.2.md](releases/v0.27.2.md) | v0.27.2: existing workflow reliability |
| [releases/v0.27.1.md](releases/v0.27.1.md) | v0.27.1: AETHER-01 proof release |
| [releases/v0.27.0.md](releases/v0.27.0.md) | v0.27.0: AETHER-01 proof release |

Where to start:

- **Planning or reviewing scope:** ROADMAP.md.
- **Planning M7 execution:** design/UPGRADE_PLAN.md. **Planning the AETHER-01
  engine proof release:** design/AETHER_01_PROOF_PLAN.md and
  design/M8_PRODUCTION_TECHNOLOGY.md. **Checking whether engine work is ready
  for game development:** design/ENGINE_FOUNDATION_PLAN.md.
- **Implementing earlier critical-path phases:** ROADMAP_DETAILS.md §B (Phase 6),
  then design/PHASE_SPECS.md.
- **Reviewing the proposed twelve-phase product gate:** design/M8_PRODUCTION_PLAN.md
  and design/m8_gates.json.
- **Redesigning the product UX:** design/AETHER_ENGINE_UX.md.
- **Building editor UI:** design/EDITOR_UI.md for visual system details;
  design/EDITOR_UX_2.md for shell details; and
  design/EDITOR_UI_TOOLKIT_ADR.md for the toolkit migration proposal.
- **Building Blueprints:** ROADMAP.md Phase 12, ROADMAP_DETAILS.md §C–D,
  and design/BLUEPRINT_NODES.md.
