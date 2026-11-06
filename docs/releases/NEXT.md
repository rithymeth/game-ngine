# Aether Engine — unreleased changes

A working draft of the next release's notes. It is rewritten into `vX.Y.Z.md` when a version is tagged; it
describes what is on `master`, not a promise of a version number or a date.
- Added **`aether_package <project.aproject> --out <dir>`**, which cooks a project and stages it with the player, its DLLs, the
  archive and a verified `PackageManifest.json` in one command. The engine already had the logic -- the editor's Build and
  Package window calls it -- but no command-line tool did, so every scripted release hand-assembled a package or adapted a
  script written for one specific game.
## Known gaps

- **Built, tested and not adopted** -- check before assuming a feature is live. The repository builds infrastructure
  before a game needs it, which is fine, but three things are in that state right now:
  - the large-world machinery (`Vec3d`, `WorldOrigin`, `RebaseTransforms`, and `streaming::FloatingOrigin`, which
    calls it) -- complete and tested, and **`FloatingOrigin::Update` is called by nothing at runtime**, so no game
    rebases;
  - `gfx/shader_cache.h`, `gfx/pipeline_cache.h` and `gfx/render_thread.h` -- tested, and the sandbox draws through a
    GPU-driven path instead;
  - **`Aether::PhysicsNet`** (`physics_net/`) -- the host-authoritative physics-over-network glue, with its own tests,
    and linked by nothing but those tests.
