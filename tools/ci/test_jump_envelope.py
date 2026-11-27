"""Run the prototype against a deterministic stand-in for the player CLI."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "jump_envelope.py"
FAKE = """#!/usr/bin/env python3
import json
from pathlib import Path
import sys

args = sys.argv
route = json.loads(Path(args[args.index("--jump-proof") + 1]).read_text())
profile = Path(args[args.index("--user-dir") + 1])
# Reused profiles would contaminate an experiment.
profile.mkdir()
assert "--jump-trace" in args
press, release = [e["frame"] for e in route["events"] if e["key"] == "Space"]
assert release - press == 2
assert [e["frame"] for e in route["events"] if e["key"] == "D"] == [0]
earliest, latest = (2, 6) if Path(args[0]).name == "before" else (4, 5)
status = "pass" if earliest <= press <= latest else "missed"
point = [0.5 if status == "pass" else 2, 0.5, 0]
if Path(args[0]).name == "unstable" and profile.name == "profile-repeat":
    point[0] = 0.75
print("Jump Route: " + json.dumps(route))
for i in range(route["frames"]):
    print("Jump Trace: " + json.dumps(dict(frame=i + 1, position=point)))
print("Jump Proof: " + json.dumps(dict(status=status, frames=route["frames"], position=point)))
sys.exit(0 if status == "pass" else 5)
"""

@unittest.skipUnless(os.name == "posix", "stand-in uses a POSIX executable script")
class JumpEnvelope(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        for name in ("before", "after", "unstable"):
            binary = self.root / name
            binary.write_text(FAKE, encoding="utf-8")
            binary.chmod(0o755)
        self.route = self.root / "recorded.json"
        self.route.write_text(json.dumps(dict(
            frames=9, fixed_hz=60, scene="Level.ascene",
            target=dict(min_x=0, max_x=1, min_y=0, max_y=1),
            events=[dict(key="D", frame=0, value=1), dict(key="Space", frame=4, value=1),
                    dict(key="Space", frame=6, value=0)])), encoding="utf-8")

    def invoke(self, player="before", *extra):
        return subprocess.run([sys.executable, TOOL, "--player", self.root / player,
                               "--route", self.route, "--key", "Space", "--press-frame", "4",
                               "--radius", "3", *extra], capture_output=True, text=True, timeout=20)

    def test_compare_early_and_late_window_without_counting_skips_as_misses(self):
        original = self.route.read_bytes()
        result = self.invoke("before", "--candidate", self.root / "after")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["baseline"]["contiguous_pass_offsets"], [-2, 2])
        self.assertEqual(report["candidate"]["contiguous_pass_offsets"], [0, 1])
        self.assertEqual(report["lost_pass_offsets"], [-2, -1, 2])
        self.assertEqual(report["gained_pass_offsets"], [])
        self.assertEqual(report["skipped_offsets"], [3])
        self.assertEqual(self.route.read_bytes(), original)

    def test_unrepeatable_reference_cannot_publish_a_receipt(self):
        output = self.root / "receipt.json"
        result = self.invoke("unstable", "--output", output)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("reference replay was not repeatable", result.stderr)
        self.assertFalse(output.exists())

    def test_existing_receipt_is_not_overwritten(self):
        output = self.root / "receipt.json"
        output.write_text("keep", encoding="utf-8")
        result = self.invoke("before", "--output", output)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("already exists", result.stderr)
        self.assertEqual(output.read_text(), "keep")

if __name__ == "__main__":
    unittest.main()
