# Aether Engine -- unreleased changes

A working draft of the next release's notes. It is rewritten into `vX.Y.Z.md` when a version is tagged, so it describes what is on `master`, not a promise of a version number or a date.

## More of the engine as plugins

Ten more optional modules are now plugins with an `.aplugin` descriptor and a runtime module that registers their components: **Animation**, **UI**, **VFX**, **Sprite2D**, **Sequencer**, **Streaming**, **Gameplay**, and the genre kits **Inventory**, **Interaction** and **Quests** (each kit depends on Gameplay, which starts first). Like Physics, Audio, Navigation, AI and Networking they are enabled by default and built when their module is, so a project can turn one off in the Plugins panel. A new engine plugin is a descriptor, a short module and one name in `plugins/CMakeLists.txt`.
