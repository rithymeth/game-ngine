# Aether Engine -- unreleased changes

A working draft of the next release's notes. It is rewritten into `vX.Y.Z.md` when a version is tagged, so it describes what is on `master`, not a promise of a version number or a date.

## Flake observed at the 0.29.0 tag

The first run on `3734ec05` failed two jobs: `Post-merge Linux (GCC)` on `bp/loop_arith_10k` at 3.25x and 3.20x against a
3.00x limit, and `Verify and record the editor screenshot` for a missing image. A re-run of the same commit passed with zero
failures, and the commit is identical.

So `bp/loop_arith_10k` (baseline 187 us) is unstable at roughly the 3x boundary on this runner, and the benchmark gate will
intermittently fail on it. The noise floor did its job -- `cook/atex_roundtrip_256` no longer appears as a regression, since its
1.8 us baseline is under the 0.01 ms floor -- but the floor does not cover a 187 us row that can triple.

Recorded rather than patched: raising the tolerance to 4x would hide it, and raising the floor to 0.2 ms would stop gating
everything under 200 us. The honest options are a per-row tolerance, or a baseline taken on the same runner, and both are bigger
than a fix made at a tag.

## Installer

The application installer (`Aether-<version>-Windows-x64.exe`, built by `tools/build_installer.py`; its source is
`tools/installer/AetherInstaller.cs`) was reworked after a report that it did not install and the old editor kept running. What it
does now: it closes any Aether program running from the install folder (asking first), moves the previous installation aside
before unpacking (all or nothing, so a file that is still open leaves the old installation intact rather than half deleted, and
the failure says so), checks that `bin\aether_editor.exe` exists after unpacking and writes `INSTALLED_VERSION.txt`, creates
"Aether Editor" shortcuts in the Start menu and on the Desktop, and offers to start the editor. Earlier installers made no shortcuts
and started nothing, so anyone launching from an old shortcut or folder kept running the old build. It takes `/silent`,
`/target:DIR`, `/noshortcuts` and `/nolaunch`. CI compiles it and runs it against a stand-in payload on Windows
(`tools/installer/test_installer.ps1`: fresh install, upgrade over a running program, a held file, shortcuts). Not covered: whether
the Qt editor it installs starts on a given machine; see the next section.


`tools/build_sdk_installer.py` builds a self-extracting Windows SDK installer from a lean Release tree. It was added for
0.29.0 and produces `dist/Aether-<version>-SDK-installer.exe`, which installs to `%LOCALAPPDATA%\Aether` without administrator
rights. The first one published is 98 MB and was verified by installing it and building a project against the result.

Two things it needed that are worth knowing: NSIS could not be installed on this machine (the winget download failed on a
certificate error), and `iexpress.exe` produced nothing usable, so the self-extractor embeds a zip payload as a managed
resource compiled with `csc.exe`. A Vulkan-enabled install also fails for a consumer that lacks the Vulkan SDK, because the
generated `AetherConfig.cmake` requires it -- which is why the installer builds with Vulkan off.

## The Qt editor crashes at startup, and CI cannot see it

Running the production Qt editor (`aether_editor.exe`, built by `aether_editor_qt`) opens no window and dies with an access
violation about forty seconds in. Captured output ends at:

    [INFO] D3D12: Selected adapter: NVIDIA GeForce RTX 3050 Laptop GPU

It reproduces from the build tree, not only from an installed copy, so it is not a packaging gap: the editor crashes while
initialising the D3D12 renderer, after adapter selection and before it creates a window.

The reason CI is green is the shape of the lane. The D3D12 editor job runs `aether_editor.exe --self-test --self-test-repeats 4`,
and the self-test exercises scene save/load, Play-in-Editor and physics -- it passes, and it never initialises the renderer. So the
only automated check of the production editor does not start the thing that crashes.

Until this is fixed, the working editor on this machine is the Vulkan ImGui shell. That is the opposite of what the naming
suggests, and it is worth knowing before a release tells anyone to start `aether_editor.exe`.

## The Qt editor crash: what I established, and where it stalls

Reproducible facts, all measured:

- `aether_editor.exe` (the Qt editor) opens no window and dies with an access violation (0xC0000005) at roughly 40 seconds.
- It reproduces from the build tree, not only from an installed copy, so packaging is not involved.
- The process's captured output ends at exactly one line: `[INFO] D3D12: Selected adapter: NVIDIA GeForce RTX 3050 Laptop GPU`.
- `--self-test` passes, because the self-test exercises scene save/load, Play-in-Editor and physics and never initialises a
  renderer.
- The crash reporter writes a dump and a text file to `%APPDATA%\Aether\crashes`, with an unresolved backtrace.
- `aether_symbolicate` cannot produce a symbolicated report on this machine: it fails with "Could not create symbolicated
  report" even when the dump is copied to a writable directory, which is the same local fault that breaks the test harness's
  scratch directory.

Instrumentation I added -- logs immediately before device creation, after device creation, around swap-chain creation, and after
command-list creation -- did not appear in the captured output at all, while the engine's own `Selected adapter` line from inside
`CreateDevice` did. So the surface code that creates the device is not where the process dies, or the process is dying before it
reaches the log, and the two observations do not yet agree. I stopped rather than guess an eleventh time.

**The next step needs a debugger attached to the Qt editor, not another edit.** Two things make that easier and are worth doing
first: make `aether_symbolicate` work on this machine so the existing dumps can be read, and add a renderer-startup path to
`--self-test` so the lane that CI runs covers the code that crashes.

## The write fault has a name: Win32 error 5, and it is the same one everywhere

Fixing `aether_symbolicate`'s report writer -- `std::wofstream` told us nothing useful when it failed, so it now buffers and
writes with `CreateFileW`, reporting the Win32 error -- produced the number that has been missing all session:

    Could not create symbolicated report: "...\aether-sym\report.txt" (Win32 error=5)

**ACCESS_DENIED**, creating a brand-new file in a directory created moments earlier, in `%LOCALAPPDATA%` and in `%TEMP%` alike.

That is the same error the test harness gets when `create_directories` builds its run directory, and the same one that makes
`Filesystem_WriteThenReadTextRoundTrips` fail. **One fault, and it is not in either piece of code**: PowerShell writes to both
directories without trouble, a .NET executable (the installer) extracted 1039 files into `%LOCALAPPDATA%` without trouble, and a
C++ executable cannot create a file in either.

So the remaining question is not about the editor, the harness or the symbolicate. It is why a native process launched from this
session's shell is denied write access to directories the same user can write to. That is one investigation, and it unblocks three
things at once: the crash dumps, the local test gate, and any native tool that writes a file.

The symbolicate change is kept anyway: reporting the Win32 error is what made this visible at all, where the old code said only
"could not create".

## A real crash in the AI utility loader, found by running outside the confinement

Running the suite in a plain administrator shell -- outside this session's process confinement -- still crashes, in a different
test than the filesystem ones:

    *** fatal signal in test: Utility_JsonRoundTripsAndRefusesBadInput
    ***   last check: !UtilityBrain::FromJson(json::parse(text), loaded, &error) (tests/test_ai_decision.cpp:128)

So this one is **not** the sandbox. It is a real defect in shipped AI code, the same class as the `StateTree::Validate` crash the
fuzzer found: a signal where `GuardJsonLoader` expects an exception.

What is already ruled out: **all eight malformed inputs are valid JSON** (checked by parsing them independently), `JsonString` and
`JsonVersion` both guard their types and cannot dereference a non-object, `GuardJsonLoader` catches `std::exception`, and reading
`FromJson` line by line shows every field access either type-checked or enclosed in the guard. None of that explains a signal,
which means the crash is somewhere the source reading does not reach -- most likely a lifetime or ownership problem in the
`Consideration`/`UtilityAction` containers rather than in the parsing.

Two attempts to instrument the loop failed to compile (the brace structure around the range-for does not survive a mechanical
patch), and the bisect only ran the previous binary. **The next step is a debugger or a standalone reproducer**, not another
edit from here -- this is the point where the session's tooling has cost more than it returned twice over.

Verified working: running the same suite in a plain shell gets **past** the four filesystem and serialization tests that fail
inside this session, which confirms those four are the confinement and not the code.

## Fixed: the utility loader crash on a numeric action name

The eight malformed documents are now eight tests, and one of them named itself in one run:

    *** fatal signal in test: UtilityLoader_RejectsANumericActionName
    ***   last check: !Loads(R"({"$type":"UtilityBrain","actions":[{"name":5}]})")

`{"$type":"UtilityBrain","actions":[{"name":5}]}` -- a numeric where a string is expected. The loader had thirteen unchecked
`json.value(key, default)` reads, and that is the defect: **`value()` throws when the stored type does not match the default's**,
so a malformed field raises a type error, and while `GuardJsonLoader` catches `std::exception`, the crash came from the one place
the guard cannot see -- the same shape as `StateTree::Validate`.

All thirteen now go through checked helpers: `JsonString` where a string is expected, a new `JsonFloat` in
`engine/include/aether/core/json_util.h` where a number is, and an explicit `is_array()` check before iterating considerations,
which previously iterated `a.value("considerations", array())` and would have thrown on a non-array too.

Verified: all eight `UtilityLoader_*` tests pass, and the crash that used to kill the process at test 128 no longer appears in the
full suite run.

Also worth recording, because it cost hours: **the local build failures were a full disk.** `D:` had 0.3 GB free of 127 GB, and
`LNK1201: error writing to program database` is what that looks like, along with `create_directories` failing in the test harness
and the symbolicate being unable to write its report. Deleting the stale 34 GB build tree took D: to 35.6 GB free and the build
worked immediately. Every "local fault" explanation in this file should be read with that in mind.

## The filesystem failures: narrowed to one clue

`fs::WriteFileBytes` returned a bare `false`, so every caller could only print "CHECK failed". It now reports the OS reason, and
that produced the line missing from every earlier attempt:

    WriteFileBytes: could not open 'C:\Users\pilot\AppData\Local\Temp\aether_test_text.txt' (errno=13: Permission denied)

**What is ruled out, each measured:**

- **Disk space** -- 34.5 GB free on D:, 46.6 GB on C:.
- **The directory** -- `%TEMP%` is writable: PowerShell creates files there, `cmd mkdir` succeeds, Python writes.
- **The filename** -- a standalone native probe creating `aether_test_text.txt`, `aether_test_bytes.bin`, `aether_zzz_probe.txt`
  and `aether_probe2.txt` in `%TEMP%` succeeds for all four with `errno=0`.
- **A leftover file** -- `aether_test_text.txt` does not exist between runs; the runner removes its scratch directory.
- **The implementation** -- `WriteFileBytes` is a plain `std::ofstream` open, and the probe uses the same call successfully.

So the identical open succeeds in a 20-line native program and fails with `errno=13` inside `aether_tests.exe`. That is the whole
remaining question, and it is narrower than "the tests are broken": it is what is different about that process's environment or
token. `errno=13` on a create in a directory the same user can write suggests an access check the probe does not meet -- a
sandbox or filter on that specific process, not on the path.

`Serialization_LegacyV1SceneStillLoads` takes a fatal signal rather than reporting a failure, which is the same write path: a
`SaveScene` that cannot open its file and then dereferences something instead of returning. Making `WriteFileBytes` honest (this
change) is what will let that test fail properly instead of crashing once the open succeeds or the caller is guarded.

## The filesystem failures are the harness process, not the engine

The runner now reports a scratch directory it cannot create, and that closed the last gap:

    scratch directory D:\aether-temp-experiment\aether-tests-21384 could not be created
    (The operation completed successfully., errno=0); writing tests will use D:\aether-temp-experiment
    WriteFileBytes: could not open 'D:\aether-temp-experiment\aether_test_text.txt' (errno=13: Permission denied)

Pointed at a brand-new directory on **D:**, created by the shell moments earlier, the test process created **nothing at all** --
the directory was empty afterwards. A 20-line native probe compiled the same way, creating the same filenames in the same
directories, succeeds with `errno=0` every time.

`create_directories` returning false with a cleared error code and `exists` then returning false is not a filesystem behaviour
this code can produce. It is what an intercepted process looks like.

**So the four filesystem and serialization failures are the harness's own process confinement, not defects in `fs::`, the
runner, or the serialization.** CI is green on the same commit for that reason.

Proof is one command in a plain shell, and it has to run **outside** this session, because the earlier outside run never reached
these tests -- it died at the utility-loader crash first, which is now fixed.

## Cooking: cooked meshes are cached

A cook that changed nothing still rebuilt the LODs and meshlets of every mesh. They now go through the derived data cache like
importer outputs and cooked textures, so a repeat cook of 1,000 models takes 3.9 s instead of 6.3 s (measured on Linux; the table
and what is still slow are in `docs/design/PERFORMANCE.md`, "Cooking at scale"). `CookReport` gains `mesh_cache_hits` and
`mesh_cache_misses`. The archive is unchanged byte for byte; delete `Intermediate/DerivedDataCache` to force a full rebuild.

## Packaging: archives are packed on several threads

Writing an `.apak` checksums and compresses its entries on all of the machine's threads (up to 16). The archive is byte for byte
what the single-threaded writer produced. On 4 cores a repeat cook of 1,000 models takes 1.65 s instead of 3.9 s, and a first
cook 4.7 s instead of 7.3 to 9.2 s. `PakWriter::SetThreads(1)` restores the serial path.
