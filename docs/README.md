# Aether documentation

| Document | What it's for |
|---|---|
| [ROADMAP.md](ROADMAP.md) | The overall plan: Phases 6–36 in dependency order, grouped into milestones M1–M6 |
| [ROADMAP_DETAILS.md](ROADMAP_DETAILS.md) | File formats, the Phase 6 (reflection) build spec, Blueprint VM design, extra editor mockups, shortcuts, estimates, risks, PR checklist, glossary |
| [design/PHASE_SPECS.md](design/PHASE_SPECS.md) | Build specs for the critical-path phases: 7 (undo/redo, GUIDs, Play-in-Editor), 8 (asset database, hot reload), 9 (prefabs, scheduling), 11 (Luau), 13 (physics events, character movement) |
| [design/EDITOR_UI.md](design/EDITOR_UI.md) | Editor UI design system: color/type/spacing tokens, icons, layouts, widgets, panel specs, interaction rules, feedback, accessibility, and the plan for splitting `editor/main.cpp` |
| [design/BLUEPRINT_NODES.md](design/BLUEPRINT_NODES.md) | Every node in the first Blueprint library with pins and behavior, compiler messages, and the node test plan |
| [tutorials/FIRST_GAME.md](tutorials/FIRST_GAME.md) | "Coin Run": how making a game will work once M2 is done, used as the M2 acceptance test |

Where to start:

- **Planning or reviewing scope:** ROADMAP.md.
- **Implementing the next phase:** ROADMAP_DETAILS.md §B (Phase 6), then
  design/PHASE_SPECS.md.
- **Building editor UI:** design/EDITOR_UI.md.
- **Building Blueprints:** ROADMAP.md Phase 12, ROADMAP_DETAILS.md §C–D,
  and design/BLUEPRINT_NODES.md.
