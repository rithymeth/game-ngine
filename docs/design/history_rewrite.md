# History rewrite: dropping the large binaries (runbook)

Status: **run on 2026-11-06.** `git filter-repo --strip-blobs-bigger-than 50M`
dropped every blob over 50 MB from all refs, one stale
`refs/codex/turn-diffs/checkpoints/...` ref holding the old history was deleted,
and `git gc --prune=now` packed the result:

| | Before | After |
|---|---|---|
| Reachable blob content | 7 241 MB | **288 MB** |
| Pack | 9.22 GiB | **2.97 GiB** |
| `.git` on disk | 9.8 GB | **3 GB** |

Every commit SHA changed (the pre-rewrite `master` was `2d65c300`, now `e1d2f5b6`);
`master`, 582 branches and 4 tags were force-pushed, and the full CTest stayed at
33/34 (`ship_tool_tests` is the pre-existing failure). The pre-rewrite repo is
preserved as a mirror clone at `E:\game-ngine-backup.git` (9.23 GB) if it ever
needs to be rolled back.

GitHub may keep the unreachable objects until its own garbage collection, so the
remote's reported size can stay large for a while even though every ref points at
the new history.

The procedure below is kept for future use.

## Why

`.git` is ~9.2 GiB packed, and **7.2 GB of that is blob content** from blobs that
are no longer in `HEAD` at all. The largest offenders:

| Size | Path |
|---|---|
| 367 MB × 4 | `games/AETHER-01/Design/AnimationScene/AETHER01_First_Contact*.blend` / `.blend1` |
| 100–149 MB each | `games/AETHER-01/Design/Meshy4K/exports/*.glb` and the same assets again as `games/AETHER-01/Content/Generated/Meshy4K/*.bin` |

None of them is tracked today (`git ls-files | grep -E '\.blend|Meshy4K|Generated'`
is empty), and `.gitignore` now keeps new copies out. They live only in history.

## What a rewrite does

- Every commit from the first commit containing a dropped blob is **rewritten**,
  so every commit SHA from there to `master` changes.
- Branch and tag pointers move to the new history.
- The old blobs become unreachable; `git gc --prune=now` then deletes them and the
  pack shrinks by roughly the dropped bytes.
- **Nothing in the working tree changes**: files, content and `git blame`
  attribution survive; only the SHAs differ.

## What it breaks

1. Every commit SHA referenced in issues, PRs, release notes, CI caches or pinned
   builds goes stale.
2. Existing clones/forks diverge; they must re-clone (or hard-reset onto the new
   history).
3. Open PRs on old commits break.
4. Signed commits lose their signatures (the commit object changed).
5. GitHub may keep the unreachable objects until its own GC, so the **remote size
   may not drop immediately**; deleting and recreating the repo is the sure way.
6. The dropped files are unrecoverable from the repo afterwards — keep the mirror
   backup.
7. Any `git worktree`, stash, or in-flight rebase/merge is invalidated.

## Procedure

Only run this when no one else is working on the repo and the tree is clean.

```powershell
# 0. Clean tree, everything pushed.
git status                     # must be clean
git fetch origin && git status -sb

# 1. Backup FIRST: a mirror clone contains every ref and object.
git clone --mirror https://github.com/rithymeth/game-ngine.git E:\game-ngine-backup.git

# 2. Rewrite. filter-repo is the supported tool (filter-branch is deprecated).
pip install git-filter-repo
cd D:\game-ngine
git filter-repo --strip-blobs-bigger-than 50M
#   Or target the paths explicitly instead of a size threshold:
#   git filter-repo --path games/AETHER-01/Design --path games/AETHER-01/Content/Generated --invert-paths

# 3. Drop the unreachable objects so the pack actually shrinks.
git reflog expire --expire=now --all
git gc --prune=now

# 4. Publish the new history (this is the destructive step on the remote).
git push --force --all
git push --force --tags
```

`git filter-repo` also removes the `origin` remote by default; re-add it if step 4
complains:

```powershell
git remote add origin https://github.com/rithymeth/game-ngine.git
```

## Verify afterwards

```powershell
git count-objects -vH                       # size-pack should be well under 3 GiB
git log --oneline -3                        # SHAs changed, messages intact
git status                                  # clean
ctest --test-dir build --output-on-failure -j 4   # still 33/34 (ship_tool_tests is pre-existing)
```

## Rollback

The mirror clone from step 1 is the rollback: delete the working copy and clone
from `E:\game-ngine-backup.git`, then force-push it back to the remote. Nothing
made after the rewrite survives a rollback, so do this before further commits.

## Alternative, if a rewrite is too much

Leave history alone and accept the 9.2 GiB. Only the local pack size is affected —
`git status`, `add`, `commit` and `diff` are already fast (~0.07 s), because the
slow part was walking untracked trees, which `.gitignore` fixed.
