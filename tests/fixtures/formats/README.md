# Settings and save-slot fixtures

`settings.asettings` (a `GameSettings` file) and `slot1.asav` (a `SaveBag` save slot) are frozen snapshots of the
file formats as they stand now (envelope `format` 1, struct `type_version` 1), written by the engine itself.
`tests/test_format_fixtures.cpp` loads them with the current code and checks every value, then loads a damaged copy and a
copy claiming a newer version, which must be refused without touching the file.

No older release's files are kept, so these guard the *next* change: if a format or a struct's version changes, keep
these files as they are, add a migration for the old version, and add the new snapshot beside them. Never edit these
files; the checksum is part of what is tested.
