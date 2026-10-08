# Aether documentation

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
| [tutorials/FIRST_GAME.md](tutorials/FIRST_GAME.md) | "Coin Run": how making a game will work once M2 is done, used as the M2 acceptance test |

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
