# Legacy file fixtures

`baseline-bd9cb13/` holds files as the tools and games wrote them at commit bd9cb13 (after v0.27.6): scenes, a Blueprint, a level sequence, input
assets, UI layouts, plugin descriptors, project files and asset metadata, copied from `games/` and `plugins/`. `tests/test_legacy_files.cpp` loads every one with the current loaders, so an engine that cannot read a
file an earlier release wrote fails the test. Never edit these files. When a format changes version, keep a
folder of the files from before the change and add a migration test beside this one.
