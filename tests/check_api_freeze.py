#!/usr/bin/env python3
"""Tests of the API-freeze checks (Phase 48 step 3): they must pass on the real tree and fail, for the right reason,
on each kind of break. Run as a CTest (api_freeze_checks)."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECK = os.path.join(ROOT, "tools", "check_api_stability.py")
FROZEN = os.path.join(ROOT, "docs", "design", "api_frozen.txt")


def run(*args):
    p = subprocess.run([sys.executable, CHECK, *args], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def expect(ok, what, detail=""):
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else "\n" + detail))
    return 0 if ok else 1


def main():
    failures = 0
    code, out = run()
    failures += expect(code == 0, "the real tree passes", out)
    code, out = run("--consumer", "samples/frozen_api_game")
    failures += expect(code == 0, "the sample game uses only stable headers", out)

    code, out = run("--consumer", "tests/fixtures/consumer_experimental")
    failures += expect(code != 0 and "aether/scene/components.h" in out and "not stable" in out,
                       "a consumer of an experimental header is refused", out)

    with tempfile.TemporaryDirectory() as tmp:
        # A baseline that remembers a name the header no longer has: a removal.
        baseline = open(FROZEN, encoding="utf-8").read().splitlines()
        edited = [l + " NoSuchFunction" if l.startswith("engine/include/aether/math/vec.h:") else l for l in baseline]
        frozen = os.path.join(tmp, "frozen.txt")
        open(frozen, "w", encoding="utf-8").write("\n".join(edited) + "\n")
        code, out = run("--frozen", frozen)
        failures += expect(code != 0 and "'NoSuchFunction' was removed" in out, "a removed name is caught", out)

        # Listing it as a deliberate removal makes it pass.
        removed = os.path.join(tmp, "removed.txt")
        open(removed, "w", encoding="utf-8").write("engine/include/aether/math/vec.h NoSuchFunction\n")
        code, out = run("--frozen", frozen, "--removed", removed)
        failures += expect(code == 0, "a listed removal is accepted", out)

        # A header that is stable but has no baseline.
        trimmed = os.path.join(tmp, "trimmed.txt")
        open(trimmed, "w", encoding="utf-8").write("\n".join(l for l in baseline if "math/vec.h" not in l) + "\n")
        code, out = run("--frozen", trimmed)
        failures += expect(code != 0 and "no baseline" in out, "a stable header without a baseline is caught", out)

    print("api freeze checks passed" if failures == 0 else "api freeze checks FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
