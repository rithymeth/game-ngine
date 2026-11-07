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

- **The Visual Studio 2022 build fails eight checks that the Ninja build passes.** Found 7 November 2026, and the first
  explanation was wrong in a way worth recording. I configured a second, empty tree and it failed eight checks where the
  long-lived tree passes 55/55, so I wrote it up as "a clean tree disagrees with an incremental one". It does not. The two
  trees use **different generators**: `build/` and `build-evervale-verify/` are Ninja, and the new tree was Visual Studio 17
  2022. The failures follow the generator, not the tree's age.
  This matters because **Visual Studio 2022 is the generator this repository's own AGENTS.md documents**, so the documented
  configuration is the one that fails: `aether_tests` plus four game suites, including
  `Filesystem_WriteThenReadTextRoundTrips` (the file reads back empty after a successful write),
  `Serialization_RoundTripsComponentsAcrossWorlds` and `Serialization_ReflectedComponentsUseDeclaredNames`, with the
  reflection registry reporting duplicate declared type names in that tree. The Ninja gate is green.
  Established by a controlled experiment, with one invalid attempt on the way. **The binary determines the result, not the
  directory and not the temp state**: the Visual Studio-built `aether_tests` reports 2/4 on the filtered filesystem checks when
  run from its own output directory, from the repository root, and from the Ninja tree's directory, while the Ninja-built
  binary reports 4/4 from all of the same three directories. Same sources, same working directories.
  The invalid attempt is worth recording because it nearly became the conclusion: I copied the Ninja binary into the Visual
  Studio tree and ran it there, and it failed -- which looked like proof that the directory was responsible. The copy changed
  `argv[0]`, and the harness resolves its run directory from that, so the copied binary was not running under the same
  conditions as the original. Re-run with the real binaries in place, the result reverses and the binary is the only
  variable left.
  Also eliminated earlier: the test temp directory (an explicit empty TEMP changes nothing) and any difference in the two
  generators' `RelWithDebInfo` command lines apart from `/arch:AVX2`, which Ninja passes and Visual Studio does not.
  **Remaining candidates, all in the build configuration.** The two generated projects for `test_filesystem.cpp` differ in
  exactly these ways, extracted from `build.ninja` and the `.vcxproj` rather than assumed:
  - Ninja defines `AETHER_HAS_EXAMPLE_PLATFORM`; the Visual Studio project does not.
  - Visual Studio defines `_ENABLE_EXTENDED_ALIGNED_STORAGE`, `JPH_*` feature macros and `CMAKE_INTDIR="RelWithDebInfo"`;
    Ninja defines the JPH set too but not the other two.
  - Visual Studio adds `AETHER_TEST_PLAYER_PATH` pointing at its own `player/RelWithDebInfo/aether_player.exe`.
  - The compiler command lines differ in `/arch:AVX2`, which Ninja passes and Visual Studio does not.
  Each of those was then checked against the source, and **none of them is referenced**: `CMAKE_INTDIR` and
  `AETHER_HAS_EXAMPLE_PLATFORM` appear nowhere in the tests or the filesystem implementation they exercise. A definition that
  nothing reads cannot change behaviour, so the four-item list above shrinks to the toolchain itself: how the same source is
  compiled, not what it is told.
  **Tested, and both are ruled out.** The Visual Studio tree was reconfigured with
  `CMAKE_CXX_FLAGS="/arch:AVX2 /permissive-"` and rebuilt from scratch (0 errors): the filesystem checks still report 2/4. The
  cause is not the compiler command line at all, and the two trees now compile the same source with the same flags and still
  disagree.
  What remains is the generator's build and link settings rather than its flags: whole-program optimisation, linker defaults,
  and the project-scope `ItemDefinitionGroup` options CMake emits for a Visual Studio solution and not for Ninja. The next
  step is to diff the `.vcxproj`'s `Link` settings and project-scope options against Ninja's link rule -- a much shorter list
  than where this started.
  Seven things have now been eliminated: tree age, the test temp directory, a copied-binary test that inverted itself, the
  differing preprocessor definitions (unreferenced in the source), `/arch:AVX2`, and `/permissive-`. Cause still not
  diagnosed.

### Two separate findings: the Visual Studio failures, and a stale primary tree

Chasing the Visual Studio failures produced a result that matters more than they do. Comparing all three build trees against
the repository:

| tree | generator | stale references to the deleted `platform_plugins/` | gate |
|---|---|---|---|
| `build/` | Ninja | **33** | 55/55 |
| `build-evervale-verify/` | Ninja | 0 | 54/54 |
| `build-clean-verify/` | Visual Studio 2022 | 0 | 8 failures |

**`build/` links object files from `platform_plugins/example/`, which no longer exists in the repository at all.** Its
`CMakeCache` still carries the configuration from when that directory was present, the object file is still on disk, and the
link line still names it. So the tree that produced every "55/55" reported in this session's earlier notes is **not a build of
the current repository** -- it is a build of the repository as it was, with deleted code still linked in. `git log -S
platform_plugins -- CMakeLists.txt` shows the configuration knob that used to add it (`AETHER_PLATFORM_PLUGINS`) still exists,
but nothing sets it any more.

The consequence is that two earlier statements in this file are wrong in a way worth keeping visible:

- "55/55" was a claim about a stale tree, not about the repository. The unaffected Ninja tree
  (`build-evervale-verify/`) is green at 54/54 and does not link the deleted plugin, so the repository's Ninja build is
  genuinely fine -- but the tree I was gating on was not a faithful build of it.
- The Visual Studio failures are real and confined to that generator: the one non-stale Ninja tree passes every check the
  Visual Studio tree fails. The two candidates left are the generator's build and link settings; the comparison that produced
  this table is the method for continuing it.

**Action before the next release:** delete `build/` and configure it fresh. Any number produced by a tree that links a deleted
source directory is not evidence about the repository, and this one had been producing the headline number all day.

### The real answer: a clean build of the repository fails, under either generator

A fresh Ninja tree (`-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAETHER_BUILD_EVERVALE_REBOOT=ON`, built with `--parallel 2`
because the default parallelism exhausts the compiler heap) produces **the same eight failures** as the Visual Studio tree:

```
aether_tests, evervale_reboot_smoke, evervale_reboot_audio_smoke,
evervale_reboot_resident_art_smoke, evervale_reboot_building_art_smoke   (8 of 54, 85% passed)
```

So the generator is not the variable after all -- the previous section's conclusion is wrong, for the third time on this one
failure. What the three trees actually show is:

| tree | faithfully built from the current sources | gate |
|---|---|---|
| `build/` | **no** -- 33 links to the deleted `platform_plugins/` | 55/55 |
| `build-evervale-verify/` | **no** -- passes the checks a clean build fails | 54/54 |
| `build-clean-verify/` (VS) | yes | **8 failures** |
| `build-fresh-ninja/` (Ninja) | yes | **8 failures** |

**The two trees that report green are both stale, and the two that are faithful both fail identically.** That means the gate
results reported throughout this session's earlier work -- every "55/55", every "all gates green" -- were measured on trees
that no longer match the repository. The repository as committed does not pass its own gate.

The failures are consistent and reproducible: `aether_tests` reports `Filesystem_WriteThenReadBytesRoundTrips`,
`Filesystem_WriteThenReadTextRoundTrips`, `Serialization_RoundTripsComponentsAcrossWorlds` and
`Serialization_ReflectedComponentsUseDeclaredNames` failing, in both generators. The four game suites fail in under half a
second each, which is a start-up failure rather than a behavioural one.

**This supersedes the two earlier explanations in this file** ("clean vs incremental", then "the generator"). Both were stories
that fit partial evidence; the fresh-tree rebuild is the measurement that settles it. Next step is unchanged in method and much
higher in priority: run `aether_tests` from `build-fresh-ninja/` and read the first four failures, which are the same ones the
Visual Studio tree showed.
