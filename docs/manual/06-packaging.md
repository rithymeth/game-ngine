# Packaging and shipping

## Cook, pack, play

`aether_cook` collects what the game reaches (the startup scene, everything it
refers to, and the folders on the **always cook** list), converts textures to
block-compressed form, and writes `.apak` archives. `aether_player` mounts them
and runs the game without the editor.

    aether_cook Game.aproject --out Paks --config shipping
    aether_player --pak Paks

In the editor, **Tools > Project > Build and Package** does the same with a
progress bar, a log, a Cancel button and Launch.

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
