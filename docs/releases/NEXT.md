# Aether Engine — unreleased changes

A working draft of the next release's notes. It is rewritten into `vX.Y.Z.md` when a version is tagged; it
describes what is on `master`, not a promise of a version number or a date.
- Added **`aether_package <project.aproject> --out <dir>`**, which cooks a project and stages it with the player, its DLLs, the
  archive and a verified `PackageManifest.json` in one command. The engine already had the logic -- the editor's Build and
  Package window calls it -- but no command-line tool did, so every scripted release hand-assembled a package or adapted a
  script written for one specific game.
- Fixed **cooking a project rewriting every imported asset's `.ameta` sidecar** even when nothing about it had changed. On a
  checkout with CRLF endings that left a freshly cooked project showing the whole `Content/` folder as modified, so a cook was
  something you had to undo in version control.
- A package now carries the engine's **third-party notices and licences** (`THIRD_PARTY_NOTICES.md`, `licenses/`), staged
  automatically and covered by `PackageManifest.json`. Previously shipping them was a manual step in one game's release script,
  so a game packaged any other way redistributed LZ4, zstd, Jolt, Recast and Luau with none of their terms.
- Added **`aether_package --smoke`**, which starts the packaged game headless for a few frames before the command returns, so
  "the package built" and "the package runs" are the same answer. The smoke run uses a scratch folder outside the package, so
  what `verify-package` checks is still what is there.
- Fixed a folder on the project's **always cook** list shipping only the files an importer claims. A licence, a readme or a data
  file a script opens by name beside a font or a model is not an asset, so it was left out of the package without a warning --
  STARDUST-HOP shipped `Roboto-Medium.ttf` without `LICENSE-Roboto.txt` even though `fonts/` was listed.
- Assets named by a **path string** are now followed by the cooker the same way GUID references are: a scene that names a
  sound cue pulls the cue in, and the cue pulls the waves it names. A folder only needs to be on the project's always cook list
  for content nothing refers to at all.
- A project now has its own **`version`**, separate from the engine version, and a package records it as `game_version` in both
  `CookManifest.json` and `PackageManifest.json`. Until now a shipped game could say which engine built it and not which version of
  the game it was, so AETHER-01's release script invented one in a side file.
- A packaged game's executable now carries **its own Windows version information** -- product name, game version, and the window
  title as its description. The player has none of its own, so a shipped `Game.exe` used to show nothing in Properties and a file
  name in Task Manager.
- **`aether_package` now ships with the editor package**, beside `aether_cook` and `aether_pak`. It was added for scripting a
  build and then left out of the one download that carries the other tools.
- The **Qt editor** — the default one — can now package a project: **File > Package Project**. It could not before, because the
  packaging window lived in the ImGui editor UI library, which the Qt editor does not link, so the only way to make a package
  from the default editor was a terminal. The worker thread, progress and log are now `PackageRunner` in the portable editor
  layer, which both editors draw, and the Qt editor's `--self-test` packages a project as part of its run.
- Fixed the **ASan+UBSan unit-test failure that blocked every release**: `RevertPrefabOverrideCommand` resolved its entity through
  `editor::Resolve`, which asserts the GUID names a live entity -- and that command's whole purpose is to report when it does not.
  Assertions are compiled out in the RelWithDebInfo build used locally, so it passed here and trapped in CI.
- A project can name an **`icon`**, and packaging stamps it into the executable's Windows icon resource, so a shipped game shows
  its own icon in Explorer, on the taskbar and in the alt-tab list instead of the default one.
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

- Fixed a **crash loading a malformed AI state tree**, found by the fuzzer. Validating a tree walked each state's parent
  chain with `Find(at)->parent`, but only the state's *direct* parent was checked for existence, so a tree whose grandparent
  was missing dereferenced a null state. It was order-dependent -- it only crashed when the child happened to be validated
  before the state carrying the broken ancestor -- which is why the fuzzer found it and the existing tests did not. The walk
  now rejects a missing ancestor, `Start` no longer dereferences a search result without checking it, and the crashing input
  is a regression test.

- A **crash now names the test and the check that died**, on both platforms. The harness already named the test from a POSIX
  signal handler, which never runs on Windows -- an assertion raises a structured exception, not a signal -- so on the
  platform most development happens on it printed nothing at all. `SetUnhandledExceptionFilter` now reports breakpoints,
  access violations, illegal instructions, divide-by-zero and stack overflows, and `AETHER_CHECK` records its expression and
  site before evaluating, so a trap prints `*** assertion trap in test: <name>` and `*** last check: <expr> (file:line)`.
- **`-DAETHER_ENABLE_ASSERTIONS=ON` keeps `AETHER_ASSERT` alive in Release and RelWithDebInfo**, where `NDEBUG` normally
  compiles it out. One required CI lane sets it, so a configuration difference that once blocked a release for several
  rounds -- an assertion that existed only in CI -- is now exercised rather than discovered.
- **Every test run owns its scratch state.** The runner points `TEMP`, `TMP` and `TMPDIR` at a private
  `aether-tests-<pid>` directory and removes it afterwards, so a file left by a failed run can no longer fail the next one
  before its logic executes. Clearing temp directories by hand is no longer part of any workflow.
- **The module-graph gate reads the source as well as the graph**, so a target declared in a `CMakeLists.txt` must have a
  layer whether or not the current configuration built it. It found two omissions immediately; the previous form passed
  locally and failed in CI.
- **`doc_links`** checks that every relative link in `docs/**/*.md` resolves, which is the check that would have noticed a
  design document cited as authoritative while it was absent from the repository.

- **A clean build tree fails eight checks that an incremental tree passes.** Found 7 November 2026 by configuring and
  building a second, empty tree from the same sources with the same cache settings (`AETHER_BUILD_EVERVALE_REBOOT=ON`,
  `AETHER_ENABLE_ASSERTIONS=OFF`) and running ctest in it: `aether_tests` plus four game suites failed, where the
  long-lived tree passes 55/55. The failing checks are `Filesystem_WriteThenReadBytesRoundTrips`,
  `Filesystem_WriteThenReadTextRoundTrips`, `Serialization_RoundTripsComponentsAcrossWorlds`,
  `Serialization_ReflectedComponentsUseDeclaredNames` and the game's smoke, audio, resident-art and building-art suites.
  The log shows the reflection registry reporting **duplicate declared type names** ("Health", "Decal", "Team", "Stats",
  "Mood", "Other", "Fighter") in the clean tree, which the serialization tests are downstream of. Disk space is not the
  cause (29 GB free). **The cause is not yet known and is not diagnosed here** -- the finding is that a release cannot be
  signed from a long-lived build directory, which is how every local gate this session was run. Reproduce by configuring a
  new empty build directory from the same sources and running the gate in it.
