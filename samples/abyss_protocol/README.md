# Abyss Protocol

**The deeper you descend, the less you can trust what you hear.**

Underwater exploration / survival mystery for Aether Engine. Stylized 3D, single-player first.

## Concept pillars

1. **The ocean listens** — Sound is both a tool and a threat. Active sonar reveals the world but attracts predators. Machinery, collisions, and movement create acoustic signatures.
2. **Darkness is a resource** — Lights improve visibility and navigation but increase detection and drain batteries. Sometimes darkness protects you.
3. **Depth changes the rules** — Greater depths increase pressure, degrade equipment, and introduce specialized creatures.

Core loop: launch from Haven-01 research hub → explore a region with submarine + EVA diver → manage acoustic signature, lights, oxygen/battery/pressure → recover data or samples → return or establish modular habitats.

## First playable target

- One compact underwater region (handcrafted, not infinite ocean).
- Controllable submarine with basic propulsion and sonar.
- Simple diver EVA mode.
- One sound-sensitive creature that reacts to acoustic events.
- One complete short investigation mission (e.g. restore a silent relay).

## High-quality 3D / asset models

Recommended approach for stylized, moderately detailed assets with strong silhouettes, bioluminescent accents, and atmospheric fog:

- **Submarine / vehicle**: Search Sketchfab or CGTrader for "stylized sci-fi submersible" or "deep sea drone" (prefer glTF/GLB with PBR). Free or CC0 options exist on Meshy.ai and Poly Haven-adjacent packs.
- **Marine life**: Hugging Face packs of 100+ deep-sea fish species as GLB (low-poly, game-ready). Also Meshy.ai underwater tags and Sketchfab CC-BY coral/reef environments.
- **Environment**: Low-poly / stylized coral, rock formations, ice, research wreckage. Sources: Sketchfab "underwater environment", CGTrader free stylized nature packs, Poly Haven textures for materials.
- **Research base / habitats**: Modular snap-point pieces. Start with simple kitbash of cylinders, spheres, and panels, then replace with high-quality models.
- Format: Prefer **glTF 2.0 (.glb)** with PBR metallic-roughness. Engine has glTF loading support via nlohmann/json and asset pipeline.
- Optimization: Keep poly counts moderate for real-time (use meshoptimizer if available). Bake lighting where possible; use emissive maps for bioluminescence.

**License note**: Always verify commercial use rights. Prefer CC0 / public domain or purchased assets for the commercial path.

## How to build (once integrated)

Add `add_subdirectory(samples/abyss_protocol)` to the root CMakeLists.txt near the other samples, then:

```
cmake --build build --config RelWithDebInfo --target abyss_protocol
```

(Windows, similar to gem_hop / coin_run.)

## Next steps

1. Integrate into root CMakeLists.txt.
2. Expand the simple ECS-style entities in abyss_protocol.cpp into full systems (acoustic propagation, pressure, light management).
3. Load real glTF models instead of placeholder geometry.
4. Add Luau or Blueprint scripting for mission logic once the player/submarine movement is solid.
5. Hook into the full renderer, physics (Jolt), and audio systems.

See the full concept from the design discussion for story (Protocol Zero, MIRA AI, Neris-9), regions, and progression.
