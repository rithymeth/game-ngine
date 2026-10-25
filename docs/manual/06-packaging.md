# Packaging and shipping

## Cook, pack, play

`aether_cook` collects what the game reaches (the startup scene, everything it
refers to, and the folders on the **always cook** list), converts textures to
block-compressed form, and writes `.apak` archives. `aether_player` mounts them
and runs the game without the editor.
Cooked textures are cached by source content and cook settings in the project's
`Intermediate/DerivedDataCache` folder. A repeat cook reuses unchanged texture
output; `aether_cook` reports how many textures were reused and cooked.

    aether_cook Game.aproject --out Paks --config shipping
    aether_player --pak Paks

In the editor, **Tools > Project > Build and Package** does the same with a
progress bar, a log, a Cancel button and Launch.

For multiplayer games, `tools/package_server.sh` packages the same cook with the
headless `aether_server` as a `.tar.gz` with SHA-256 checksums; see
[Dedicated servers](15-dedicated-server.md).

## Packaged audio

The player runs the existing `AudioSource`, `AudioListener` and `ReverbZone`
components, sound cues (`.acue`) and sequence audio keys. Cue names and wave names
are content-relative paths, such as `Audio/shot.acue` and `Audio/shot.wav`.
WAV, Ogg Vorbis and FLAC waves are streamed from the package. Put the cue/wave
folders and sequence folders on the project's **always cook** list: references
stored as path strings are not GUID dependencies followed by the cooker.

Windowed players start the platform audio output, with the existing null backend
when no device is available. Headless runs render audio on the frame thread;
they do not open a device. Master/Music/SFX/Voice volumes follow player settings.
Invalid packaged waves are reported during scene load; cue diagnostics are
available from the audio system. Animation sequence keys still require a player
animation host.

C++ hosts can inspect `Game::AudioSystem()` after loading a scene. Call
`Game::StartAudioOutput()` to enable device output. Replacing the scene stops the
old output; call it again for the new scene. Scheduled gameplay kit stages follow
the replacement scene while custom stages registered through `Game::Systems()`
remain registered.

## Strict release cooking and package checks

For a release candidate, add `--strict` to `aether_cook`, or check **Fail on
cook warnings** in Build and Package. Strict validation reports every warning
and fails before writing a new archive. Missing roots, broken asset references,
failed imports and unreadable helper files are among the warnings that block a
strict cook. Importer setting warnings include the asset source path and also
block a strict cook. A normal cook still reports warnings without failing, so they can
be investigated during development. Packaging stages the cooked content,
player, DLLs and manifest before installation. A staging or installation
failure restores the previous package files.
Cancellation is honored after cooking and before each installation step. A
cancelled update keeps the previous package; cancelling after the final
manifest has been installed does not undo a completed package. Packaging
refuses linked destinations and unexpected file types: `Paks` must be a
directory, and the player, DLLs and manifest must be regular files. This
prevents an update from deleting a same-named directory containing user files.
If rollback itself fails, the error identifies the backup folder retained
for recovery. Do not run concurrent updates into the same package directory.

The packaged folder includes `PackageManifest.json`, listing the player,
copied DLLs, archive and cook manifest with byte counts and CRC-32 values.
Run `aether_pak verify-package <packaged-directory>` to detect missing or
changed files. Use `aether_pak verify <archive.apak>` to check the archive's
entries too. CRC-32 detects accidental damage; it does not authenticate a
release or replace signing.
`aether_cook --pak-name <name>` accepts a single filename stem; the output
directory is supplied separately with `--out`. Names containing path separators,
NUL characters or `:` drive/stream syntax are rejected before cooking.
Verification rejects linked manifests and linked parents of listed files,
including a linked `Paks` folder. Manifest entries must use normalized relative
paths without NUL characters or `:` stream/drive syntax, and reference regular
files. Repeated paths are rejected, including ASCII case variants on Windows.
Verification covers the listed files; it does not reject unrelated extra files
or protect against files being changed concurrently with verification.

## Configurations

**Debug** logs everything; **Development** logs info and a stats line every
second; **Shipping** logs only warnings and errors and drops editor-only data.

## Project settings

The window's title, size and vsync, the startup scene, the fixed timestep,
physics layers and the quality presets (resolution scale, shadow cascades and
resolution, MSAA) are in **Project Settings** and are cooked into the game; the
player picks the default preset or one named with `--quality`.

## Patches, DLC and encryption

- `--patch-of Game.apak --pak-name Game_p1` writes only what changed since a base
  archive; a patch named to sort later wins, and can remove files.
- `--dlc Name` cooks the always-cook roots as a DLC, leaving out what the base
  already has; mounting it adds its assets.
- `--key <64 hex digits>` encrypts an archive (ChaCha20); the player opens it
  with `--key`, `AETHER_PAK_KEY`, or a key built in at compile time.

## Plugins

A project's **Plugins** tool lists the engine's and the project's plugins and
turns them on; the cooker records which, and the player starts their runtime
modules before the first scene loads.

## Game releases, patches and rollback (Linux, Windows and macOS)

`tools/ship/aether_ship.py` (Python 3 standard library only) packages a cooked
game for release and installs, patches and rolls it back on the player's
machine. `tools/ship/build_release.sh --build build --out dist` runs the whole
AETHER-01 flow: a strict shipping cook, the package, and a sample patch.

    aether_ship.py package --build build --cooked Paks --version 1.0.0 --out dist
    aether_ship.py install --archive AETHER-01-linux-x64.tar.gz --sums SHA256SUMS.txt --dest ~/Games/AETHER-01
    ~/Games/AETHER-01/AETHER-01            # the launcher; extra arguments go to aether_player
    aether_ship.py verify --install ~/Games/AETHER-01
    aether_ship.py make-patch --from v1/AETHER-01-linux-x64.tar.gz --to v2/AETHER-01-linux-x64.tar.gz --out patch.tar.gz
    aether_ship.py apply --install ~/Games/AETHER-01 --patch patch.tar.gz --sums SHA256SUMS.txt
    aether_ship.py rollback --install ~/Games/AETHER-01
    aether_ship.py check-symbols --install ~/Games/AETHER-01 --symbols AETHER-01-linux-x64-symbols.tar.gz

`package` writes:

- `AETHER-01-linux-x64.tar.gz`: `bin/aether_player` (stripped), the launcher,
  `Paks/*.apak`, `CookManifest.json`, notices, `licenses/` and `GAME_MANIFEST.json`.
- `AETHER-01-linux-x64-symbols.tar.gz`: the player's debug info, split with
  `objcopy --only-keep-debug`, at `.build-id/xx/rest.debug` (the layout gdb's
  `debug-file-directory` uses), plus `SYMBOLS.json`.
- `AETHER-01-linux-x64.manifest.json`: the release manifest, with the package and
  symbols archive hashes, and `SHA256SUMS.txt`.

`GAME_MANIFEST.json` lists the project, version, `build_id` (a hash of every
installed path and its SHA-256), git revision, engine and minimum engine
version, the toolchain (from the build's CMake cache), the player's shared
library dependencies, licenses, the paks in mount order (by name; later paks
win), symbols with their GNU build-ids, and every file's size and SHA-256.
Packaging the same build and cook twice gives byte-identical archives (sorted
entries, fixed owner, mtime from `SOURCE_DATE_EPOCH`). Fresh cooks from clean
copies are byte-identical too (sub-asset GUIDs are derived, not random).

A patch holds only the files that changed (usually an `aether_cook --patch-of`
delta pak), the files it removes, and the new manifest. `apply` checks the
archive against `SHA256SUMS.txt` (when given), the installed files against
their manifest, that the patch was built from the installed `build_id`, every
patch file against the new manifest, and the minimum engine version when the
patch keeps the installed player. It then patches a copy (`<dir>.staging`),
checks the copy against the new manifest, and switches it in with two
renames, keeping the old version as `<dir>.previous`. A refused patch leaves
the install unchanged. `rollback` swaps `<dir>` and `<dir>.previous`, so you
can run it again to return to the update. If the machine stops between the
two renames, the next `apply` or `rollback` finishes the switch.
The player writes saves and settings to the user folder (`--user-dir`, or the
OS's folder), outside the install. Files inside the install that the manifest
doesn't list (a `Saved/` folder, for example) are copied over on both apply and
rollback.

Install the system Vulkan loader (`libvulkan1` on Ubuntu) to run the player,
including `--headless`. The manifest's `runtime_dependencies` lists the
libraries the player needs. On a tagged release, the Release job adds the three
files above to the release's `SHA256SUMS.txt` and provenance attestation. On
every CI run, the `game-ship` job tests install, launch, patch, rollback and
refused patches in a bare `ubuntu:24.04` container
(`tools/ship/ship_smoke.sh`), then assembles the release files without
publishing them.

### Windows

The same commands work on Windows with `--platform windows-x64`
(`build_release.sh` runs under Git Bash; set `PYTHON=python` if `python3` is
not on the path). Configure the MSVC build with
`-DAETHER_MSVC_RELEASE_PDB=ON` so Release links with `/DEBUG` and writes
`.pdb` files.

    build_release.sh --build build --out dist --platform windows-x64
    python aether_ship.py install --archive AETHER-01-windows-x64.zip --sums SHA256SUMS.txt --dest "%LOCALAPPDATA%\Games\AETHER-01"
    "%LOCALAPPDATA%\Games\AETHER-01\AETHER-01.exe" --headless --frames 60 --report

The differences from Linux:

- The package, symbols archive and patch are `.zip` files (no Unix modes or
  owners, so they stay byte-identical across machines).
- The player is `AETHER-01.exe` at the package root, next to its DLLs and
  `Paks/`. There is no shell launcher; the manifest's `launcher` is the exe.
  `runtime_dependencies` lists the DLLs it imports that the package doesn't ship.
- `-symbols.zip` holds the `.pdb` named by each binary's CodeView (RSDS)
  record, in symbol-store layout (`aether_player.pdb/<GUID><age>/aether_player.pdb`),
  so the unzipped folder works as a symbol path in Visual Studio or WinDbg.
  `SYMBOLS.json` records each exe/DLL with its PDB GUID and age. `package`
  reads the GUID from the PDB info stream and the age from the DBI stream, and
  refuses a PDB that doesn't match the exe. `check-symbols` repeats that check
  against the installed binaries. A DLL with no `.pdb` (third-party) ships
  without symbols and a warning.
- The install is switched with folder renames (`os.replace`), never links. If
  the game is running, or something else holds a file in the install open,
  Windows refuses the rename: `apply` and `rollback` then stop with
  "a file in it is in use" and leave the install as it was. If it fails between
  the two renames, the next `apply` or `rollback` finishes the switch.

The Windows CI job builds this package as `AETHER-01-windows-x64-ship.zip`
(the existing `AETHER-01-windows-x64.zip` from `tools/package_aether01.ps1` is
unchanged). It runs the same `ship_smoke.sh` install, launch, patch, rollback
and refused-patch checks natively, and the Release job adds the package,
symbols and manifest to `SHA256SUMS.txt` and the attestation. The Windows game
package is not Authenticode-signed; that stays the separate, optional
`windows-sign-release` job.

### macOS

`--platform macos-arm64` (Apple Silicon) packages an app bundle. Build
`RelWithDebInfo` (the dSYM needs the debug map) and have the Xcode command-line
tools installed (`dsymutil`, `strip`, `install_name_tool`, `codesign`); the
install, verify, patch and rollback half still needs only `python3`.

    build_release.sh --build build --out dist --platform macos-arm64
    python3 aether_ship.py install --archive AETHER-01-macos-arm64.tar.gz --sums SHA256SUMS.txt --dest ~/Games/AETHER-01
    ~/Games/AETHER-01/AETHER-01.app/Contents/MacOS/AETHER-01 --headless --frames 60 --report
    open ~/Games/AETHER-01/AETHER-01.app

The differences from Linux:

- The package is still a `.tar.gz` (it keeps the executable bits) with one top
  folder, which holds `AETHER-01.app`, the notices, `licenses/` and
  `GAME_MANIFEST.json`. Install it as a folder and keep the bundle inside it:
  `verify`, `apply` and `rollback` work on that folder (`<dir>.previous`,
  `<dir>.staging` as on Linux), not on the bundle alone.
- Inside the bundle: `Contents/MacOS/AETHER-01` (the stripped player; it is the
  manifest's `launcher`), `Contents/Info.plist`, `Contents/Resources/Paks/`
  (the player looks in `../Resources/Paks` next to its executable on macOS),
  and `Contents/Frameworks/` with every non-system dylib the player loads.
- `package` reads the player's Mach-O load commands itself (pure Python,
  thin or fat): `LC_LOAD_DYLIB` (and weak/re-export/lazy/upward variants),
  `LC_RPATH`, `LC_ID_DYLIB` and `LC_UUID`. Libraries outside `/usr/lib` and
  `/System` (Homebrew's `libvulkan.1.dylib`, for example) are resolved
  through `@rpath`/`@loader_path`/`@executable_path`, copied into
  `Frameworks/` (transitively), and renamed `@rpath/<file>`. Build-machine
  rpaths are deleted and `@executable_path/../Frameworks` is added. When the
  Vulkan loader is bundled, MoltenVK is bundled too (found through
  `VK_ICD_FILENAMES`/`VK_DRIVER_FILES` or Homebrew's `MoltenVK_icd.json`), with
  its ICD manifest in `Contents/Resources/vulkan/icd.d/`, a folder the loader
  searches inside app bundles. Packaging fails if anything still loads a
  library from outside the bundle. `runtime_dependencies` lists the system
  libraries and frameworks it loads, and the manifest's `binaries` list every
  Mach-O in the bundle with its `LC_UUID`.
- Symbols: `dsymutil` writes `<UUID>/AETHER-01.dSYM` before the player is
  stripped with `strip -S -x`. `-symbols.tar.gz` holds the dSYM and
  `SYMBOLS.json`. `package` and `check-symbols` compare the binary's `LC_UUID`
  with the UUID of the dSYM's DWARF Mach-O, and `check-symbols` also checks
  every bundled dylib's UUID against the manifest. In lldb, use
  `target symbols add <UUID>/AETHER-01.dSYM`.
- `strip` and `install_name_tool` invalidate the linker's signature, and arm64
  refuses to run unsigned code, so every Mach-O is re-signed ad hoc
  (`codesign --force --sign -`, no identity, deterministic).
- APFS is case-insensitive by default: manifests already refuse two paths that
  differ only by case, and nothing in the install relies on case.

The macOS CI job builds `AETHER-01-macos-arm64.*`, runs the same
`ship_smoke.sh` natively (with the Homebrew Vulkan driver environment unset),
does the release assembly dry run and uploads `aether01-macos-arm64-ship`. On
a tag the Release job adds the package, symbols and manifest to
`SHA256SUMS.txt`, the attestation and the published assets.

Not covered yet: the app is not signed with a Developer ID or notarized (the
repository has no Apple signing secrets). A downloaded copy carries the
`com.apple.quarantine` attribute, and Gatekeeper blocks it ("cannot be
opened"). Right-click > Open, or `xattr -dr com.apple.quarantine
~/Games/AETHER-01`, lets it run. Installing with `aether_ship.py` from a
quarantined archive doesn't copy the attribute, since Python extracts the
files itself. Patching changes files in `Contents/Resources`, which would
break a sealed (Developer ID) signature; a signed release would need to be
re-signed after each patch, or keep its paks outside the bundle. Only arm64
is built (no x86_64 or universal binary).

## Verifying an engine download

Each GitHub release lists `SHA256SUMS.txt` and `RELEASE_MANIFEST.json` next to the zips. Every file also carries a
signed build-provenance attestation, which shows it was built by this repository's CI from the tagged commit:

```sh
sha256sum -c SHA256SUMS.txt                                   # the zips match the published sums
gh attestation verify AetherEditor-windows-x64.zip --repo rithymeth/game-ngine
```

The Windows executables are not Authenticode-signed yet (`"signed": false` in the manifest), so Windows SmartScreen
may warn the first time you run them.
