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
