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

### The filesystem failure, measured

Instrumented `Filesystem_WriteThenReadTextRoundTrips` in the fresh tree rather than reasoning about it again, and the probe
contradicted the shape I had inferred:

```
CHECK failed: fs::WriteFileBytes(path, content.data(), content.size())
[PROBE] path=C:\Users\pilot\AppData\Local\Temp\aether_test_text.txt exists=0 size=0
        temp=C:\Users\pilot\AppData\Local\Temp\
```

Two things, both concrete:

1. **The write returns false.** It does not "succeed and vanish" -- `WriteFileBytes` cannot open the file at all. The
   previous note said the write reported success and the read-back failed; that was inferred from the failure lines without
   the write's own check visible, and it was wrong.
2. **The harness's private run directory is not in effect.** `temp_directory_path()` returns the real
   `C:\Users\pilot\AppData\Local\Temp`, not the `aether-tests-<pid>` directory the harness is documented to point
   `TEMP`/`TMP`/`TMPDIR` at before the first test and remove afterwards. So the redirect that is supposed to isolate every run
   is not happening in this build.

That means the write is failing against the real temp directory, and the isolation the harness documents is absent. Either is a
bug on its own; together they explain the check without explaining why the open fails, which is the next question and needs the
`errno` the stream sets rather than another guess.

### The harness run directory is never created, and the environment is not why

`main` does call `PrepareRunDirectory()` and `PointTempEnvironmentAt()`. A probe inside `PointTempEnvironmentAt` printed
**nothing**, so it returned at its first line: `PrepareRunDirectory()` returned an empty path. That is why the isolation is
absent and why the filesystem test wrote to the real temp directory and failed.

What has been checked about the environment, all from PowerShell in the same session:

- writing `aether_test_text.txt` to `C:\Users\pilot\AppData\Local\Temp` succeeds;
- creating a directory there succeeds;
- no stale `aether_test*` files are present, though two stale `aether-tests-<pid>` directories from earlier crashed runs are.

So the directory and its permissions are fine and the failure is inside the process, in `PrepareRunDirectory`'s three fallible
steps: `temp_directory_path(error)`, the `remove_all`, and the `create_directories`. The next step is to print those three
`std::error_code` values, which turns the remaining question into a single line of output rather than another hypothesis. This
is the last inference in the chain that is worth writing down before measuring -- six previous ones on this failure were wrong.

### The deny ACE was real, was removed, and is not the cause

The error codes named the mechanism: `temp_directory_path` succeeded, and `create_directories` failed with
**error 5, Access is denied**. Walking up the ACL chain found a `Everyone : DeleteSubdirectoriesAndFiles (Deny)` ACE defined
on **`D:\game-ngine` itself**, inherited by every directory created underneath it. `build/` and `build-evervale-verify/` were
created before that ACE existed and do not inherit it; every tree created since does. That is why two faithful trees failed
identically and the two stale ones passed, and it looked like the answer.

Removing the ACE succeeded (`icacls D:\game-ngine /remove:d Everyone`, deny count now 0, and a freshly created nested
directory inherits no deny). **The filesystem test still fails with the same checks, and `WriteFileBytes` still returns
false.** So the ACE was real and worth removing -- new build trees should not inherit a deny -- but it is not what makes this
test fail.

That is the eighth explanation for this failure and the eighth to be withdrawn. What is now known without inference:
`temp_directory_path` succeeds; `create_directories` for `aether-tests-<pid>` returns error 5 in this process even though
PowerShell creates the same kind of directory under the same parent without trouble; and the same binary passes when run from
`build/tests/` and fails when run from `build-fresh-ninja/tests/`. The next measurement is the process token -- integrity level
and privileges -- rather than another filesystem hypothesis.

### What is left after nine eliminations

Reverted the reflection renames, rebuilt the fresh tree, and the filesystem checks still fail. So it is not those edits either.
Cleared every deny ACE from the tree (count 0, inherited and explicit) and re-ran: still fails. Ran the same binary from four
different working directories: fails from all four, so it is not the directory. The two tests directories contain the same six
files.

The eliminations now stand at nine: tree age, generator, compiler flags `/arch:AVX2`, `/permissive-`, the differing
preprocessor definitions, the test temp directory, a copied-binary test that inverted itself, the deny ACE on the repository
root, and the reflection renames.

What remains is the simplest statement available and the one worth acting on: **a fresh build of the current sources fails these
four checks, and a tree whose object files predate some commit passes them.** The two differ only in when their objects were
compiled, so the next tool is `git bisect` with a scripted `configure && build --parallel 2 && ctest -R "Filesystem|Serialization"`,
not another hypothesis. Nine guesses have been wrong; a bisect cannot be.

Two facts are worth carrying into that bisect because they cost the most to learn:
- `WriteFileBytes` returns false because `create_directories` for the harness's run directory fails with error 5, Access is
  denied, **in the test process** while PowerShell creates the same directory under the same parent without difficulty.
- The default Ninja parallelism exhausts the compiler heap on this machine; use `--parallel 2`.

### The bisect range, bounded by a passing binary

The last tree that passes is `build-evervale-verify/` at **54/54**, whose `aether_tests.exe` was linked at **06:57** and whose
`filesystem.cpp.obj` dates from the previous evening. The source state at that moment is **`3d7ad154`** (06:44). HEAD is
**24 commits** later and a fresh build of it fails four checks.

Between those two states, **`engine/src/platform/filesystem.cpp`, `tests/main.cpp` and the filesystem header are unchanged** --

```
git diff --stat 3d7ad154 HEAD -- engine/src/platform/filesystem.cpp tests/main.cpp .../filesystem.h
(no output)
```

-- so the regression is not in the code the failing checks exercise directly, and not in the harness that sets up their
directory. That narrows the bisect further than the commit range alone: whatever changed, it changes the environment the test
process runs in, not the filesystem calls it makes.

The tree is clean of deny ACEs (0 on the repository root, the tree and the tests directory), PowerShell writes into both the
fresh tree and the temp directory without trouble, and the test process still gets **error 5** from `create_directories`. So the
bisect is the right next tool and it has a known-good endpoint: `3d7ad154`.

Recommended bisect script, because a plain `git bisect run` will not do here:

    cmake -S . -B build-bisect -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAETHER_BUILD_EVERVALE_REBOOT=ON &&
    cmake --build build-bisect --parallel 2 --target aether_tests &&
    AETHER_TEST_FILTER=Filesystem_WriteThenReadText ./build-bisect/tests/aether_tests.exe

`--parallel 2` because the default parallelism exhausts the compiler heap; the target is `aether_tests` alone because that is
where the failing checks live and it cuts each step to a few minutes rather than the full tree's fifty.

### The bisect was invalid, and why every "passing" tree is suspect

`git bisect` ran four steps and named **`f8a9011c` -- "Record the developer-facing changes in the release notes"** as the first
bad commit. That commit touches one markdown file. It cannot break a filesystem test, so the bisect's premise is false and its
answer is worthless.

The premise was "`3d7ad154` is good", and that was taken from `build-evervale-verify/` passing 54/54. That tree is **not a
comparable build**:

| tree | configuration | deps fetched | verdict |
|---|---|---|---|
| `build-evervale-verify/` | editor, editor UI, bench, fuzzers all **OFF** | 5 | 54/54 pass |
| `build-fresh-ninja/` | default (everything on) | 42 | 4 fail |
| `build/` | default, but stale -- links the deleted `platform_plugins/` | -- | 55/55 pass |

Neither passing tree is a build of the repository as anyone would configure it: one is a minimal subset, the other still links
code that no longer exists. **Every faithful, full build fails the same four checks.** So the bisect range was wrong before it
started, and the failure predates everything in this session's commit range.

The next bisect must use the **same options** on both endpoints -- in practice, `git bisect` with a script that configures with
the default options and builds `aether_tests` only, and a first step that verifies the assumed-good commit actually passes in
that configuration. That last check is the one I skipped, and it is the reason four bisect runs were spent proving nothing.

### The endpoint was never good: the failure is configuration-dependent and local

Tested `3d7ad154` -- the commit the bisect assumed was good -- in the same default configuration the bisect would build. It
**fails the same filesystem checks**. So the bisect's good endpoint was never good, the range was inverted from the start, and
the four bisect steps were measuring nothing. The failure predates this session's entire commit range.

What that leaves, tested rather than inferred:

- **`build-fresh-ninja/`**, default configuration (editor, editor UI, bench, fuzzers on): **fails**.
- **`build-evervale-verify/`**, minimal configuration (those four OFF): **passes**.
- The two test binaries are **31 MB and 55 MB** -- different test sets, so they are not running the same program.
- **CI's Windows lane passed the full configuration** on `3e4aa6af`, so the full configuration is not intrinsically broken.

So this is **configuration-dependent and local**: a build with the editor and its dependencies enabled fails a filesystem check
that a build without them passes, on this machine, while CI's full build passes. That is a very different claim from "the
repository does not pass its own gate", which is what three earlier note sections said. The repository's gate is green in CI; what
is broken is one local configuration.

The next step follows from the binary-size difference rather than from another hypothesis: find which test translation units the
55 MB binary contains and the 31 MB one does not, and check the filesystem check in that configuration with the harness probe
that produced the error codes. That is a diff, not a guess.

### The diff, done: 58 extra translation units, and it is not test order

Compared the test objects between the two configurations:

| configuration | test objects | `test_filesystem.cpp.obj` | result |
|---|---|---|---|
| minimal (editor/bench/fuzzers off) | 158 | present, 783087 bytes | **passes** |
| default (everything on) | 216 | present, 783075 bytes | **fails** |

The same test object is in both, at the same size, and the 58 extra objects are all editor, bench, build-window and other
non-core tests. Every test the minimal binary has, the default one has too -- the sets are nested, not different.

That rules out test ordering as the cause, and the earlier evidence was already against it: the failing check was run through
`AETHER_TEST_FILTER=Filesystem_WriteThenReadText`, so only that one test executed in the failing binary. A single filtered test
that fails while its identical object passes in another binary means the difference happens **before any test runs** --
static initialisation or link-time state contributed by one of those 58 objects, not a test that ran earlier and left a mess.

That turns the next step into a finite list rather than another hunt: configure the default build with `AETHER_BUILD_BENCH=OFF`,
`AETHER_BUILD_EDITOR=OFF` and so on one flag at a time, or link each of the 58 in turn, and the one that flips the result names
itself. It is the first time in this investigation that the candidate set has a known size.

### The flag bisect is invalid: the features are coupled into the test target

Ten `AETHER_BUILD_*` flags differ between the passing minimal tree and the failing default one (devtools, editor, editor UI, MCP,
net, physics, sandbox, scripting, terrain, Vulkan -- all OFF in the minimal tree, all ON in the default one). Bisecting them
looked like the obvious next step, so I turned six of the ten OFF and rebuilt: **the build fails with four errors**, because the
test target's sources require those features.

So the flags cannot be bisected individually, and the only configuration comparison this repository supports is the two extremes:
everything on, or everything off. That is why the failure has resisted every intermediate experiment -- there is no valid
intermediate configuration to test.

What is left is the method the diff actually supports: a **link-order bisect over the 58 extra test translation units**. Add them
to the 158-object build a few at a time and the one whose presence flips the result names itself. That is finite, it does not
require a valid intermediate *configuration*, and each step is a link rather than a full build.

Not done here. The session has produced the narrowing -- 158 versus 216 objects, the same test object in both, a difference that
happens before any test runs -- and the next step is mechanical with a known size.

### The retraction was wrong: the repository does fail its own gate

Turned every differing feature flag OFF in the fresh tree -- devtools, editor, editor UI, MCP, net, physics, sandbox, scripting,
terrain, Vulkan -- and rebuilt. The build succeeds, and the test target still contains **216 objects**. The minimal tree's 158 is
not a configuration at all: it is **stale**. Its `tests/CMakeLists.txt` predates 58 test files that exist in the repository now.

So the two "passing" trees are both stale in different ways:

| tree | objects | why it passes |
|---|---|---|
| `build/` | 55/55 | links the deleted `platform_plugins/` directory |
| `build-evervale-verify/` | 158 | **predates 58 test files** |
| `build-clean-verify/` | 216 | **fails** |
| `build-fresh-ninja/` | 216 | **fails** |

**The repository as committed fails 4 of 54 checks in a full build.** That was three note sections ago and I retracted it after
seeing that CI passed -- but CI passing and this failing are not in conflict, because CI and this machine are not building the
same set of 58 translation units. The retraction was premature: I generalised from "CI is green" to "the configuration is fine"
without checking that the object counts matched.

What is now established with no inference left:
- A full build of the committed repository fails `Filesystem_WriteThenReadBytesRoundTrips`,
  `Filesystem_WriteThenReadTextRoundTrips`, `Serialization_RoundTripsComponentsAcrossWorlds` and
  `Serialization_ReflectedComponentsUseDeclaredNames`.
- The failure happens **before any test runs** (a single filtered test fails), and the test object is identical in the passing
  and failing binaries, so the difference comes from another object's static initialisation or link-time state.
- The candidate set is the 58 test translation units the stale tree never compiled -- **not** any feature flag, since turning
  them all off changes nothing.

The next step is a link bisect over those 58, starting with the ones that register reflection state at namespace scope.

### The link bisect has its method, and it is a CMake edit

The 58 extra translation units are not in the `add_executable(aether_tests ...)` list. The first few are; the rest arrive through
`target_sources(aether_tests PRIVATE ...)` blocks, most of them inside `if(...)` guards that also define the `AETHER_TEST_HAS_*`
macros the guarded tests depend on. My first attempt to comment them out matched nothing, which was itself informative: the test
target is assembled in three different ways across one file, so a mechanical edit has to remove a **guard and its
`target_sources`**, not a filename.

So the link bisect is a sequence of edits to `tests/CMakeLists.txt`: disable the guard blocks a few at a time, keeping their
macros consistent with the sources still compiled, and the block whose removal flips the filesystem check names the culprit. It is
mechanical and finite, it is larger than it looked, and it is the right next piece of work.

Nothing is left to infer. The repository fails four checks in a full build; the difference happens before any test runs; the test
object is identical in both binaries; the candidate set is 58 translation units; and the tool is an edit to one file.

### The candidate set is eight, by static-initialisation reach

Ranked the 58 by whether they can run code before `main`: eight declare a namespace-scope object whose initialiser references
`std::filesystem` -- `test_build_window`, `test_compat_corpus`, `test_editor_projects`, `test_extensions`, `test_loc_editor`,
`test_save_inspector`, `test_script_reload`, `test_vcs`. That is consistent with the failure, which is a filesystem operation
(`create_directories`) that runs in the process but fails with Access denied while the same call from PowerShell succeeds.

My attempt to look at *what* each initialiser does produced the same line for all eight, because the regex matched the type name
rather than the call. So the eight are ranked, not distinguished.

Next step, now small enough to be a single sitting: disable those eight `target_sources` entries in four-and-four and let the
filesystem check say which half holds the culprit. Three builds at fifteen minutes each. Everything else in this investigation is
either established or eliminated.

### Neither half of the eight contains it, so the static-initialisation filter was wrong

Disabled `test_build_window`, `test_compat_corpus`, `test_editor_projects` and `test_extensions`, rebuilt: **still fails**.
Swapped to `test_loc_editor`, `test_save_inspector`, `test_script_reload` and `test_vcs`: **still fails**.

Both halves exclude the culprit, so it is not among the eight files whose namespace-scope objects touch `std::filesystem`. That
makes the reasoning that produced the eight wrong, not merely incomplete -- the failure is not caused by an initialiser in those
files.

That is the twelfth explanation to fall and the first one where a *filter* produced a set that provably excludes the answer. The
candidate set goes back to all 58, and the next bisect has to be over the full set rather than a ranked subset.

The honest summary of this failure after a full session: a full build of the committed repository fails four checks before any
test executes; the test object is identical in passing and failing binaries; 58 translation units differ between the only passing
and the only failing builds; and four separate attempts to narrow those 58 -- by feature flag, by compile flags, by configuration
and by static initialisation -- have each been wrong.

