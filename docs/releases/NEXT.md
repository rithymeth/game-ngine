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

## Drag across a plane

The Move gizmo has three small squares between its arrows: grab one to slide the object in two directions at once (along the floor, for example) in one drag. They follow the World/Local
choice, snap each direction on its own, and are one undo step. A press right on an arrow still takes the arrow.

## Move and rotate along an object's own axes

The Move arrows and Rotate rings can follow the object's own axes instead of the world's: the World/Local button in the viewport toolbar, or Q. A rotated object can now be
pushed along its own length with one arrow. With snapping on, Local snaps the distance moved (the world grid does not line up with a tilted axis). World is still the default.

## Click anywhere on a model to select it

Picking was by an entity's origin (a click had to land within 14 pixels of it, so a big model could not be picked by clicking on it). The Scene View and the Live
Viewport now read each model's bounding box from its glTF file (`ModelBoundsCache`, kept until the scene is opened or the viewport is (re)started) and pick by a ray
through the mouse: the nearest box it passes through is selected, and an entity with no model (a camera, a light, an empty) is still picked by its origin, over a model behind it.
The selection outline is the model's real box instead of a one-metre cube. It is still the box, not the shape: clicking beside a thin or concave model but inside its box selects it;
pixel-exact picking would need the renderer to write an object-id buffer.

## Select and move things in the rendered view

The Live Viewport takes the Scene View's editing tools: click an object in the rendered picture to select it (ctrl adds or removes), drag a gizmo handle to
move, rotate or scale it (W, E, R, with snapping), and the selected object is outlined, all drawn over the real renderer's picture and following your drag. The tools
are one shared piece (`ViewportTools`) used by both views, so they behave the same. Objects are picked and outlined by their models' real bounding boxes (see the next section), not by the pixels of their shape.

## The Live Viewport shows your edits as you make them

The Live Viewport no longer needs the scene saved: **Start** cooks the models the open scene uses and hands the running game the scene **as it is open**
(`GamePackage::SetContentOverride`, with the scene serialized the way Play-in-Editor snapshots it), and after that every change (an edit, a gizmo drag, an
undo) reloads it a moment later (`SceneDocument::Revision` says when it changed; a drag in progress is announced with `Touch`). Nothing is written over the
project's scene file. The game's world is frozen by default so the view is the scene as authored (**Run game** lets physics and scripts run); a model added
since the last cook is said in the toolbar and appears after **Reload**. A move is patched in place (see the next section); any other change reloads the whole scene (the world, physics and scripts start over), which is
fine for small and medium scenes.

## Dragging in the Live Viewport no longer restarts the game

Every change used to load the whole scene again, so dragging an object with **Run game** on also restarted physics and scripts on every frame of the drag. Now, when only
Transforms changed (a gizmo drag, a nudge, a typed position), the editor puts the new Transforms into the running scene (`LiveViewport::PatchTransforms`, matched by entity ID) and
leaves everything else alone; the game keeps its own state. The document says which kind of change it was (`SceneDocument::StructureRevision`); adding or removing an
entity, editing any other component, or an undo still loads the scene afresh. Measured, it also costs less: at 960x540 with 2,000 entities an edit takes about 77 ms instead of about 116 ms (see PERFORMANCE.md).

## A live rendered viewport, and two fixes it needed

The editor has a **Live Viewport** tool: the project's scene running under the real renderer inside the editor itself (the player's `Game` and
`SceneRenderer` on the editor's own Vulkan device, an offscreen swap chain read back every frame), with the Scene View's orbit camera. **Start** saves the
open scene, cooks the project and loads it; **Reload** does it again to see edits. It shows the project as last cooked (a running game, not a paused edit view).

Two bugs this turned up, both in the Scene tool:

- **Saving could replace the wrong scene.** A new, untitled scene saved to the default name (`Scenes/Main.ascene`) overwrote the project's real Main scene
  without a word, and the Render and Start buttons save first, so pressing them in a project did it. A save now never replaces a file that exists and is not the
  open scene: it is refused with a message, and only choosing Save As again on that name (or opening the scene first) goes ahead.
- **The Scene tool could not open a template project's scene.** Components of the other modules (a `SkyLight`, Blueprint instances, colliders, scripts) were not
  registered in the editor process, so the scene was refused (correctly, since dropping them would lose data on save). The editor now registers every
  built-in component when it starts (`RegisterEditorComponents`).

## Top-down BMP images load

A BMP stored top-down (negative height in the file, as the player writes its screenshots) was refused as "invalid" by `DecodeImageFile` and
`DecodeImageFromMemory` (stb's `stbi_info` reports that height negative), so a texture in that form could not be imported. It loads now, with
the rows in the right order. Found while building the editor's **Render Preview**, which renders the open scene with the real renderer and
shows the picture in the editor.

## The Qt editor is removed; the editor is Dear ImGui

At the project owner's request the Qt editor (`editor/qt`, target `aether_editor_qt`, option `AETHER_BUILD_QT_EDITOR`) is gone, and
`aether_editor_shell` (Dear ImGui over Vulkan) is the editor. Removed with it: its `--self-test` ctest, its Windows packaging and
the Qt install steps in CI, the Qt coverage ledger and UX roadmap, and the Qt concept page. `aether_editor_core` and the document
classes stay (the ImGui panels and the MCP tools use them). Costs, stated plainly: no 3D viewport in the shell (a project: the scene renderer draws a cooked package through a running game into a swap
chain), and the Windows package no longer contains an editor, only the build tools, the player and the assets (the hosted Windows
runner has no Vulkan loader), so the package validation job now checks that those are present instead of launching an editor.
`tools/installer` still looks for `bin\aether_editor.exe`; that name needs to change to `aether_editor_shell.exe` with a Windows
run of `test_installer.ps1`, which could not be done here. The two sections below describe the Qt startup crash and are kept as the
record; the crash no longer applies.

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

## Cooking: textures are compressed on several threads

BC1, BC3, BC4, BC5 and BC7 textures of 256x256 and up are block-compressed on all of the machine's threads (up to 16). The cooked
texture is byte for byte the same, so existing caches stay valid. On 4 cores a 2048x2048 BC7 texture cooks in 1.8 s instead of
6.0 s. ASTC is unchanged. `TextureCookSettings::threads = 1` restores the serial path.

## Scenes: destroying or making a prefab from a large subtree is no longer quadratic

`Lifecycle::Destroy` and `MakePrefab` scanned the whole world once for every entity under the one they were given. They now find
all children in one pass (`ChildIndex`, also available to games): destroying a 20,000-entity subtree takes 5.7 ms instead of
238 ms, and 50,000 entities 12 ms instead of 1.7 s. A parent cycle in damaged data now ends instead of looping. `SubtreeOf`
returns an entity and everything under it.

## Rendering: drawing entities in a hierarchy costs less transform work

The player's draw loop reads each entity's local transform once a frame instead of once for every drawn entity under it. For
10,000 entities in a tree the transform work falls from about 2.0 ms to 1.3 ms a frame; scenes without hierarchies cost what
they did. Transforms are bit for bit the same, with or without interpolated physics poses. `ExtractRenderScene` (used by tools
and tests, not by the player) got the same treatment: 10,000 objects in a tree, 5.7 ms to about 3 ms. `HierarchySnapshot`
(`scene/gameplay.h`) is available to games that ask about many entities at once.

## Onboarding: a project from the command line, and templates that show something

`aether_new <Name> --template <id>` creates a project from a template without the editor (`aether_new --list` shows them) and
prints the two commands that package and run it. A project made from the First Person, Third Person, Top Down or Vehicle template
used to run to a black window, because the templates held no models: they now generate a floor, a player body and pickups from one
cube in three colours. All six templates were created, packaged and run this way on Linux. Two still open on a dark window: Blank,
by design, and 2D Platformer, because the packaged player does not draw sprites or tilemaps. The getting-started page has the
three commands.

## Animation: mistyped graph parameters are diagnosed

Invalid animation graph parameter commands, failed montage playback, and unavailable montage-section jumps now report the
entity and request once per distinct problem. Repeated gameplay requests no longer fail silently or flood the diagnostics list.

## Scene saves keep a recoverable previous version

Binary and JSON scene saves now replace their target atomically and retain the prior file as `<scene>.bak`. If a save is
interrupted, the original scene remains intact; the previous version can also be loaded directly for recovery.

## Packaged projects render Sprite2D content

The packaged player now turns scene sprites and tilemap layers into the same textured quad batches used by the renderer's particle
pass. This closes the gap where 2D template scenes loaded and simulated correctly but their sprites and tiles were invisible at runtime.

## 3D templates start with procedural environment lighting

First Person, Third Person, Top Down, and Vehicle projects now include a procedural sky light. Their first scene has an environment
background and ambient image-based lighting without relying on an imported sky texture.

## Sprite2D draws authored 2D lighting

The packaged renderer now samples the Sprite2D light map for sprites and tilemaps when a scene contains enabled 2D lights. Point,
spot, and global ambient lights affect the packaged Sprite2D pass, with tilemap occlusion and box shadow casters applied by the light map.

## 2D: sprite animation clips play in packaged games

A `SpriteAnimator` did nothing in the packaged player: no stage advanced it, so an animated sprite stayed on one frame. The game
now runs `Player.SpriteAnimation` each frame after scripts and Blueprints, reading sprite atlases from the package on first use.
Looping clips loop, a clip that does not loop stops on its last frame and sets `finished`, and a clip the atlas lacks leaves the
sprite as it is.

## 2D: a character that falls out of the level can be put back

`PlatformerController2D` has `respawn_on_fall` and `fall_limit_y`: with the first set, a character that drops below the limit is
put back where it stood on its first step, at rest (`respawns` counts how often). It is off by default, so existing games are
unchanged. The 2D Platformer template turns it on: its level has a pit, and a first-time player holding right used to lose the
character for good.

## Player: `--press` keys stay held in a windowed run

`aether_player --press <Key>` set its keys once at start. A window that loses the keyboard clears every held key, which always
happens under a virtual display, so windowed smoke runs had no input after their first frame. The keys are now set again every
frame, as "held for the whole run" says.

## Plugins: `aether_plugin new`, and a scaffold that no longer warns at every launch

`aether_plugin new <Name> [--dir <parent>] [--module]` creates a plugin folder without the editor. The scaffold (also what the
editor's Plugins window makes) used to declare a runtime module named after the plugin, although it had no code: a project that
enabled it got "The module 'Coins' isn't built into this player" at every launch. It is content only now; `--module`, or
`CreatePluginScaffold(..., with_module = true)`, declares the module for a plugin that has code. A plugin scaffolded before this
change keeps the module line in its `.aplugin`; delete it if the plugin has no code.

## Rendering: drawing many entities is about 15 percent cheaper on the CPU

The player built each drawn node's normal matrix by inverting its whole 4x4 transform, every frame. It now computes the
inverse transpose of the linear part directly, about 40 times fewer instructions. Recording a frame of 20,000 identical entities
fell from 21.8 to 18.8 ms on the software Vulkan driver (1,000 entities: 1.05 to 0.90 ms). Lighting is unchanged to within
rounding of the packed half floats the normals are sent as; the matrix agrees with the old one to 2e-5. A frame of 20,000
drawn entities still costs more than a 60 Hz frame: each entity is its own draw call, and the RHI has no instanced draw yet.

## AETHER-01: the LastLight Valley environment renders

`LastLight_Valley.bin` in the repository was 300,062 bytes where its glTF declares 330,324, so the model failed to load and the
environment drew as nothing, with only an `[ERROR] glTF` line in the log. It is regenerated by the game's own builder (the rest of
the builder's output was already identical to what was committed). The builder also wound the river ribbon facing down, so the
meltwater was culled from above; it faces up now. `games/AETHER-01/tools/validate_assets.py`, which catches the length mismatch,
now runs as the `aether01_assets_validate` check; before, nothing ran it.

## Screen-space ambient occlusion

The player's post pass darkens the HDR scene where the depth copy shows nearby surfaces closing in: 12 golden-angle taps in a sphere
of 2.5 m, normals rebuilt from depth, strength 0.9, applied before exposure and the tone mapper. It needs the HDR path and the scene
depth copy (the same one temporal anti-aliasing reads). `AETHER_RENDER_AO=0` turns it off; `AETHER_RENDER_AO_STRENGTH` (0 to 1) and
`AETHER_RENDER_AO_RADIUS` (metres) tune it. It is measured only by eye on the LastLight Valley under lavapipe (0.4 of a level off the
mean, 1 to 2 % of pixels visibly changed, at the cliff base, under rocks and on the bridge); there is no GPU-time figure yet and no
automated test of the image. Known limits: it also darkens directly lit surfaces (it multiplies the final colour), the noise is not
blurred, and thin geometry does not occlude.

## Distance fog

`PostProcessVolume::fog_density` (1 / m, default 0: off) fades the scene toward the sky's colour with distance from the camera, `1 -
exp(-density * distance)`, in the post pass from the scene depth copy. It blends between volumes like the other settings. The sky is
left alone. No height fog or sun in-scatter yet, and transparent draws are not fogged (they are not in the depth copy).

Fog also takes the sun's light toward the sun (in-scatter), as 10 % of its colour by default; `AETHER_RENDER_FOG_SUN` changes the share.

## Procedural terrain generator

`aether::terrain::GenerateTerrain(TerrainGenSettings)` builds a heightmap of mountains and valleys (domain-warped fractal noise blended
with ridged noise), erodes it (thermal slides, then hydraulic droplets that conserve what they move), fills depressions into lakes,
cuts rivers where enough water collects, and classifies the result into biomes (water, beach, desert, grassland, forest, rock, snow) from
height, slope, moisture and temperature. `BuildTerrainSplatmap` turns that into the four-layer weights the terrain renderer takes (grass,
rock, sand, snow); `RenderTerrainPreview` draws a shaded biome picture. Same settings and seed, same terrain, on any machine. About
700 ms for 513 x 513 with 200,000 droplets, single-threaded. No MCP tool or asset format yet.

## Environment scatter

`aether::terrain::Scatter(terrain, settings, rules, scatter_settings)` places trees, shrubs, grass, rocks and debris over a generated
terrain by rules: biomes, height, slope, distance from water, a patchiness noise, density, clumping, scale, tilt to the ground, and the
footprint each object takes (later rules keep clear of earlier ones). Deterministic from the seed; each rule has its own random stream.
`DefaultScatterRules()` is a starter set; `ToFoliageInstances` converts the result for the foliage layer. About 44,000 instances in
38 ms on a 513 x 513 terrain. No MCP tool or rule asset yet.

## Procedural meshes

`aether::assets::procedural` generates rocks, cliff faces, pine, broadleaf and dead trees, houses and towers, and modular walls, pillars, arches
and stairs from parameters and a seed, as the engine's own `MeshData` (bark, leaves, glass and so on as separate primitives with materials).
`GenerateVariant(kind, index, seed)` makes a different piece for every index, so hundreds of unique rocks or trees are a loop. Deterministic;
triangle budgets per kind (a pine about 400, a broadleaf about 5,000, a rock 300 to 1,300). `EncodeGlb` writes a `.glb`; `ValidateMesh` and
`ComputeStats` check a mesh. 600 pieces in about 0.8 s. No LODs, no MCP tool yet.

## Procedural World editor

A new World panel, **Procedural World**: every setting of the terrain generator and the scatter rules as sliders with a live preview, four presets (temperate,
alpine, desert, islands), a seed box and dice, undo, and a mesh family per scatter rule. **Apply** sends the generated terrain to the Terrain editor and the scatter
to the Foliage editor; **Bake** writes `Content/Generated/<name>/` with the terrain as a model, a `.glb` per mesh variant and a scene of model entities the player
loads. The generator got three fixes found by rendering a bake (less spiky erosion, flat lakes and sea, a smoothing setting). Grass, reeds and cacti have no mesh
generator yet and bake as nothing.

## Plants

`procedural_mesh.h` gains shrubs, grass tufts, reeds with cattail heads and saguaro-style cacti (`GenerateShrub`, `GenerateGrass`, `GenerateReeds`, `GenerateCactus`,
and the kinds `Shrub`, `Grass`, `Reeds`, `Cactus` for `GenerateVariant`). The Procedural World editor's starter scatter rules now bake with them. A bake's object
budget goes to the rules in order, so trees keep every instance and the grass is thinned to fit what is left.

## Textured terrain in baked worlds

A bake paints the terrain: `terrain::RenderTerrainTexture` colours it by height, slope, moisture and temperature with smooth blends (rivers painted blue, lake and sea beds
dark), and the baked model is the textured ground plus flat water for lakes and the sea (`BakeOptions::terrain_texture`, 1024 by default, 0 for the old flat
surfaces). `assets::EncodePngRgb8` writes PNGs. Engine fix: the cook, the importer cache key and content moves now follow a `.glb`'s external images (they
only did for `.gltf`), and `RewriteGltfUris` can repack a `.glb`.

## Shorelines and a terrain skirt

In baked worlds the water of a lake or the sea runs on under the shore, so the visible shoreline follows the ground's real contour instead of whole quads, and the terrain
gets a skirt (`BakeOptions::terrain_skirt`, 6 m by default, 0 to leave the edge open) that carries its edges down to a base, so it reads as a block.

## Water with depth and foam

A baked world's lakes and sea carry a texture (`terrain::RenderWaterTexture`, `BakeOptions::water_texture`, 512 by default, 0 for flat blue): turquoise shallows to dark blue
deep by the water's real depth, and a lacy white foam line along the shore. No renderer change.

## See-through, rippled water

A baked world's water blends (RGBA texture: 30 percent opaque in the shallows to 93 in the deep, foam solid) and carries a ripple normal map (`RenderWaterNormalMap`), both
through the renderer's existing BLEND and normal-map support. `EncodePngRgba8` writes RGBA PNGs; `EncodeGlb` writes `alphaMode` and `normalTexture`. The water does not
move yet: that needs a time input in the scene shader.

## Moving water, and a time input for the scene shader

The scene shader's frame constants carry the game's time. A glTF material with the `AETHER_materials_water` extension scrolls two layers of its normal map with it, so baked
water ripples and glints. The exporter writes the extension, the bake sets it, and the ripple map tiles so there is no seam. Other effects (swaying grass, flicker) can
read `g_Frame.time.x` too.

## Swaying vegetation

A glTF material with the `AETHER_materials_wind` extension (metres of sway at the top, 0 to 0.3) sways in the vertex shader with the game's time, weighted by height so roots stay put,
with a phase from where each instance stands so a field moves like a gust. The procedural generators set it on grass, reeds, shrub and tree leaves (a `sway` setting on each, 0 for
none). Shadows do not sway yet.

## A global Wind

A `Wind` component (direction in degrees about up, strength 0 to 4, gust speed, gustiness) sets the wind every swaying material follows; the first in the world is used, a world without one keeps the
old breeze. The direction is in the world whatever way an object faces, and the sway is in world metres whatever its scale (a scaled-up tuft used to sway further).

## Baked procedural worlds include a Wind

`BakeOptions` gained `add_wind`, `wind_direction_degrees` and `wind_strength` (on by default); the baked scene gets one
`Wind` entity. The Procedural World panel exposes them on the Output tab.

## Wind zones

New `WindZone` component: a sphere with its own wind that replaces the world's `Wind` inside it, fading over `falloff`. Up
to 7 zones; `FrameData` is now 1376 bytes. An alpha-mask material's cutoff is stored in 8 bits instead of a half.

## Swaying vegetation casts a swaying shadow

`ShadowCaster` and `ShadowCasterConstants` carry the sway (112 bytes of push constants). Wind-flagged casters with a
non-zero wind are drawn into the shadow cascades only, like skinned meshes; virtual shadow pages under them fall back to
the cascades.

## Render textures with depth

`IDevice::CreateRenderTextureWithDepth` and `PipelineDesc::render_texture_depth`: a colour render texture with its own D32
depth attachment, cleared by every `BeginTexturePass`. Vulkan only for now; D3D12 returns an invalid handle.

## Water reflections

Water mirrors terrain and non-swaying objects (Vulkan). `FrameData` is 1392 bytes. The procedural world baker writes one
water primitive per level. `AETHER_RENDER_REFLECTIONS=0` disables it.

## Recording from the player

`aether_player --camera-path <json> --record-dir <dir>`: the scene's camera follows keyframes and every frame is written
(see `player/src/camera_path.h` for the format).

## Fixed: a second, lower water surface inside the sea

A basin under the sea that spilled lower than the sea had its own lower water level, which left a band of ground standing
out of the sea. Its water now stands at the sea's level. Worlds baked before this need a re-bake.

## Terrain detail layer

New glTF extension `AETHER_materials_detail` (`strength` 0 to 1): a procedural micro-normal and brightness variation from world
position, fading out with distance. Procedural-world terrain uses it. An alpha-mask cutoff is now 5 bits.

## Height fog

`PostProcessVolume::height_fog_density`, `height_fog_falloff`, `height_fog_base`: exponential height fog that pools in valleys and
over water. The post pass's push constants are 128 bytes.

## Foliage tint variation

Plants (materials with wind) take a small per-instance colour tint from their world position, so forests and grass fields are not
one colour. No setting.

## Clouds

`SkyLight::cloud_coverage`, `cloud_scale`, `cloud_speed`: procedural clouds in the sky, lit by the sun and drifting with the Wind.
Off by default. `FrameData` is 1408 bytes.

## Clouds in the water

`SkyPass::DrawMirrored`: the sky (clouds, sun) is drawn into the water's reflection, so the sea mirrors the clouds. Vulkan only.

## Water swells

Water (`AETHER_materials_water`) lifts and tilts with the wind's strength: three travelling swells in the vertex shader. A world with
no Wind component has the default breeze, so its water now has a gentle swell; set the Wind's strength to 0 for flat water.

## Whitecaps

Swells foam on their crests in a strong wind (a wind strength over about 0.7). No setting beyond the Wind component.

## Filming a scene's sequences

`aether_player --record-dir <dir> [--record-fps <n>] --frames <n>` writes the frames of a normal run at the film's frame rate, so a scene's own sequences are filmed
without a camera path. `--camera-path` still works alone or with it.

**A real oak.** `ProceduralKind::OakTree` (and `TreeParams::crown_spread`, 1 for the old behaviour) makes a short thick-trunked tree whose limbs reach sideways before they rise, so the crown is wider than the tree is tall. The Procedural World editor's starter set has an "Oak" rule that uses it. It is a new kind, appended, so existing documents keep their kinds. Across 12 variants the oak's crown is 1.58 times as wide as it is tall and the birch's 1.31 (test `ProceduralOakIsABroaderTreeThanTheBirch`).

**The player stops drawing what the camera cannot see.** The scene pass used to submit every draw every frame. It now leaves out a draw whose bounding sphere is outside the camera's view or smaller than a quarter of a pixel (`player/src/view_cull.h`); skinned meshes, water, shadow casters and the water mirror's draws are not affected. A baked HeroValley (6,001 draws) goes from 266 to 156 ms a frame on the software rasterizer with identical output; a GPU is not measured. `AETHER_RENDER_CULLING=0` turns it off.

**Far levels for swaying plants.** A shrub or tree of 100 or more triangles has a second, cheaper index list over the same vertices (`player/src/mesh_lod.h`), drawn, and used for its cascade shadow, when it is under 24 pixels in radius on screen. The scene pass is about 7% cheaper in a plant-heavy view on the software rasterizer (frame -4%; not measured on a GPU); the picture changes in under 4% of its pixels. `AETHER_RENDER_LOD=0` turns it off. `AETHER_PLAYER_GPU_TIMING=1` makes the player print the GPU time by stage each frame. The earlier culling note's -41% was a best case; with plants in view it is -6% of the frame.

**Baked terrain in tiles.** The Procedural World baker cuts the ground into 8 by 8 primitives (`BakeOptions::terrain_tiles`; 1 keeps one piece) with `assets::procedural::SplitIntoTiles`, which any primitive can use: it copies the vertices on a cut with their normals, so no seam shows. One primitive had one bounding sphere around the whole map, so neither the camera nor a shadow cascade could ever skip any of it. On a baked HeroValley the frame is 11 to 14% cheaper on the software rasterizer (the shadow pass 14%), with an identical picture; a GPU is not measured. Bakes made before this keep their single piece until baked again.

**Baked worlds have collision.** The Procedural World baker adds a static triangle-mesh collider over the ground (`BakeOptions::terrain_collision`, on by default where the editor is built with physics; `terrain_collision_grid`, 192 quads a side, sets how fine), so a `CharacterMovement` capsule stands on and walks over a baked terrain. On HeroValley it adds 0.5 to 0.7 s to the player's start and about 8 MB to the source scene (the cooked pak is binary); the mean gap between the collision mesh and the heightmap is 6 cm. Bakes made before this have none until baked again.

**Baked worlds are playable.** `BakeOptions::add_player` (with `terrain_collision`) adds a Player running the Third Person template's controller, a small figure for a body, a camera, a sun and a sky, on dry gentle ground (`ProceduralWorldDocument::FindSpawnPoint`), plus invisible walls round the map's edge so a character cannot walk off it. New: `templates::InstallThirdPersonKit(content, ...)` writes the controller and bindings into any project (existing files are kept). The Procedural World panel has the two checkboxes and, after a bake, says the project must cook `Input/`. Tested by baking a world and playing it headless: it lands, walks, jumps and the camera follows. The figure is static and the camera has no obstruction handling.

**The third-person camera stops at the hill.** The Third Person controller sweeps a sphere from the character's head toward the camera and keeps the camera in front of whatever it hits (a wall, a hill), coming in at once and easing back out; `camera_collision` and `camera_radius` are script options, off-able, and a build without physics keeps the old behaviour. **The baked world's player walks**: its figure is a body and four limbs that swing with its pace (`Scripts/LimbSwing.luau`, installed by `templates::InstallThirdPersonKit`, which any jointed figure can use), and `ProceduralWorldDocument::BuildPlayerModel(PlayerPart)` makes the parts.

**The scene writer is proven against a killed process.** A new test starts a child that saves the scene and its autosave in a loop and kills it with SIGKILL 100 times at random moments (about one in ten lands mid-write); the saved scene, its backup and the autosave are whole every time, and the same test against a non-atomic writer fails. Row A03 of the readiness matrix is updated: the document layer is proven, the Qt editor's restore prompt is not (Qt is not built where this ran). No engine code changed.

**Import failures say what is wrong.** The model importer now reports, in `ImportResult::warnings` (and the new `GltfScene::warnings`), every primitive it skipped and every attribute it dropped, naming the mesh, the primitive and the cause (before, such a file imported as "ok" and empty); refuses an index past the vertices; requires a glTF 2.x `asset.version`; gives the JSON parser's line and column, "the file is empty", and which buffer file is missing and where it looked; and `LoadGltfFromMemory` has an overload that returns the reason. The texture importer says what a file is when it cannot read it: a Git LFS pointer, an HTML page, a format it does not import (WebP, TIFF, DDS...), text, or a PNG, JPEG or BMP that is truncated or over the pixel budget (told apart by the size in its header). A good file still says nothing.

**A scene with a component the build does not know is refused by name, not stripped.** The MCP `load_scene` (and `aether_mcp_server --scene`) used to skip an unknown component with a log line, and a later `save_scene` deleted it from the file; they now refuse, naming each unknown component, and `allow_unknown_components=true` loads the rest knowingly and returns `skipped_components` and a warning. `SceneLoadOptions::unknown_components` collects the names for any caller, and the editor's refusal names them too. **Fixed:** the editor renamed an old scene's unnamed entities again ("Entity 2" to "Entity 2 2") the second time the scene was opened. New: the legacy fixtures are round-tripped through the editor's open and save, with nothing lost or changed.

**Script errors say which entity and where, and do not flood.** A Luau error now ends with the callback and the entity (`(in OnUpdate, on entity "Front Door" a0b1c2d3)`), so one broken script on forty doors can be told apart; the instruction-budget error for an endless loop says the file and line it was stuck on (`Spinner.luau:4:`), like every other script error; and an error that repeats (a broken `OnUpdate` fails every frame) is one entry that counts its repeats, logged at 1, 10, 100 times and so on, instead of one entry per frame for the whole session. `ScriptSystem::Errors()` keeps at most `kMaxDistinctErrors` (200) distinct errors; `DroppedErrors()` counts the rest.

**The portable editor has a Scene tool.** `aether_editor_shell` (Dear ImGui over Vulkan, which runs wherever Vulkan does, headless included) gains **Scene**: a hierarchy tree (filter, select, rename, create, duplicate, copy and paste, delete, drag to parent, with cycles refused), an inspector (every reflected component, add and remove, all undoable) and a toolbar (new, open, save, undo, redo, play, with a prompt before discarding unsaved changes), on the same `SceneDocument` the Qt editor uses. `SceneDocument::Context()` and `Commands()` expose its command stack. The Qt editor is unchanged and still behind `AETHER_BUILD_QT_EDITOR`. No 3D viewport in the shell yet.
