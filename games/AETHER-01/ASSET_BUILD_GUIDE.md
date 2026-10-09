# Last Light Valley build guide

## Layout and scale

- World bounds: X/Z = -48 to +48 m; vertical variation is intentionally strong at the perimeter cliffs.
- Entry: south edge (Z = -48); first amber wayfinder at (-4, -18).
- River corridor: winding central meltwater channel; keep a clear walkable route around both banks.
- Mid-route markers: (3, -5) and (-2, 12).
- Transition: transit bridge at Z ≈ 29 m; gate and citadel at Z ≈ 37–45 m.
- Silhouette pieces: 30 tapered basalt spires and 65 talus shards, placed with seeded variation for repeatable builds.

## Scene assembly layers

1. **Terrain** — triangulated canyon basin and glacial water ribbon.
2. **Primary silhouettes** — rim fangs and canyon spires.
3. **Route landmarks** — bridge piers/deck, gate monoliths, citadel terraces and towers.
4. **Navigation accents** — three amber obelisks and bridge signal rings.
5. **Gameplay layer to author in editor** — walkable collision, player start, streaming bounds, encounters, checkpoints and exits.

All layers are named nodes in `LastLight_Valley.gltf` to simplify later conversion into separate modules or streamed cells. The deterministic source builder is the canonical editable procedural source for this first environment pass.
