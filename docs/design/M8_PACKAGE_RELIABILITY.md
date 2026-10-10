# M8 package reliability increment

Scope: the existing cook, package, verify and release workflows. This work
adds no engine subsystem and makes no Aether 1.0 readiness claim.

## Supported behavior and defects

| Existing workflow | Baseline limitation | Improvement | Regression evidence |
|---|---|---|---|
| Update a staged package | A directory at the executable/manifest/DLL path could be backed up and then deleted as if it were an old file | Validate all destination types before installation and recheck each swap; preserve unexpected user files | `Package_RefusesDirectoryWhereAFileIsExpected` |
| Cancel Build and Package | Cancellation after the cooker finished could still publish the new player and package | Honor cancellation during staging and before each installation step, using the existing rollback path | `Package_CancelAfterCookKeepsThePreviousPackage` |
| Verify package contents | Checking the final file alone followed linked parent folders; a linked manifest was accepted | Inspect every relative path component and require regular files | `Package_VerifierRejectsLinkedParentFoldersAndManifest` |
| Verify manifest paths | Embedded NUL could be truncated by native calls; Windows case variants could name the same file twice | Reject NUL and drive/stream syntax and duplicate ASCII case variants on Windows | `Package_VerifierRejectsDamagedAndMalformedFiles` |
| Name a cooked archive | A path separator in the archive name could write outside the selected output or staging folder | Require a single filename stem before scanning/cooking | `Package_InvalidArchiveNameCannotEscapeStaging` |
| Recover from an install failure | Staging failure had a test, but failure after replacing `Paks` did not | Exercise a locked Windows executable after content installation and compare restored files byte for byte | `Package_LockedPlayerRollsBackInstalledContent` |
| Bundle release notes | Both ZIP paths copied notes for a fixed old release | Select notes from the checked-in engine version and fail if that document is absent | Windows package workflow and standalone package script |

## Verification contract

Keep `PackageManifest` at version 1 and CRC-32 as its integrity check. No
serialized-format migration is required. Verification covers listed files,
not extra files or authenticated provenance. It is not a lock against
concurrent file modification. Concurrent packaging into one directory remains
unsupported; an interrupted process is not equivalent to a recoverable API
failure. If rollback cannot restore a file, retain the backups and report their
location instead of deleting the last recoverable copy.

Ordinary Windows accounts may lack symlink privileges. The symlink regression
prints that limitation when applicable; Linux/macOS CI must exercise the case.
The locked-executable rollback case runs on Windows only. The existing platform
CI matrix, physics-on/off unit checks and AETHER-01 package/playthrough gates
remain the release requirements.

M8's broad production gates remain `n/a yet`: this increment establishes
targeted package correctness evidence, not external developer, polish,
performance, signing or complete production-platform acceptance.

## Local evidence recorded 2026-10-08

The pre-fix package implementation passed 3 of 7 targeted checks; the four
failures reproduced late cancellation, directory replacement, malformed path
acceptance and linked parent/manifest acceptance. The existing Windows
locked-executable rollback check passed. With the fixes and archive-name
validation, all 20 focused package, cook and texture checks pass, including
all eight package checks. The symlink cases ran on this machine without a
privilege skip.

Environment: Windows 11 Pro, MSVC 14.44, Ninja, RelWithDebInfo, physics enabled,
Vulkan enabled. This is correctness evidence; no performance improvement or
target-hardware frame-rate claim is inferred from it. Logs are retained under
the ignored build folder (`upgrade-package-baseline-tests.log` and
`upgrade-package-tests.log`).

The full local build succeeds. CTest passes 20/20 entries, including
1,066/1,066 unit tests and 2/2 functional scenarios. A CLI probe also rejects a
Windows junction at `Paks` without changing its target files.

On 2026-10-10, a Windows working-tree recheck cooked the current AETHER-01
project into two independent Development output folders. Both `Game.apak`
files had SHA-256
`51adba07b35c1a5d3b05b277cf59c983ffa6c4d70776e79fac59642e1519a7ad`; both
`CookManifest.json` files had SHA-256
`fda0c681e632a129f2e91d1e5c3bb511111b4a5930c67c107d304101e37a9137`. The
second cook reported four reused textures and zero recooks. The standalone
player loaded that archive for 120 frames and reached `MissionStage` 5. This
working-tree check is not clean-checkout or cross-platform evidence. The
Windows CI workflow now repeats the cook, checks cache reuse and both hashes;
that CI evidence is pending its next run.

Two strict Development cooks of AETHER-01 produce identical archive and
cook-manifest bytes. The local archive SHA-256 is
`8f680f38fb4c737d343cf036748b6359e68145a397271312d647a7349e941dec`;
the unchanged second cook reuses its one texture. The standalone package
includes the v0.27.2 notes. From an empty working directory, headless and
rendered replay reach MissionStage 5, Health 2 and ArchiveTime 8; an ending
capture passes the pixel check. A fresh process restores MissionStage 5 and
Health 3. Canonical release archive hashes must be taken from the CI/released
artifacts: local working-tree line endings can differ from tagged source.
