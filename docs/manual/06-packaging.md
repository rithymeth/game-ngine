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
strict cook. A normal cook still reports warnings without failing, so they can
be investigated during development. Packaging stages the cooked content,
player, DLLs and manifest before installation. A staging or installation
failure restores the previous package files.

The packaged folder includes `PackageManifest.json`, listing the player,
copied DLLs, archive and cook manifest with byte counts and CRC-32 values.
Run `aether_pak verify-package <packaged-directory>` to detect missing or
changed files. Use `aether_pak verify <archive.apak>` to check the archive's
entries too. CRC-32 detects accidental damage; it does not authenticate a
release or replace signing.

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
