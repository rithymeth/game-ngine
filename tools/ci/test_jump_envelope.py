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
earliest, latest = (2, 6) if Path(args[0]).name in ("before", "same") else (4, 5)
status = "pass" if earliest <= press <= latest and Path(args[0]).name != "always-miss" else "missed"
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
        for name in ("before", "after", "unstable", "same", "always-miss"):
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

    def test_release_gate_fails_with_receipt_even_when_original_route_passes(self):
        output = self.root / "gate.json"
        result = self.invoke("before", "--candidate", self.root / "after",
                             "--require-preserved-offsets", "--output", output)
        self.assertEqual(result.returncode, 5, result.stderr)
        self.assertEqual(result.stdout, "")
        report = json.loads(output.read_text())
        self.assertEqual(report["gate"], {"policy": "preserve-pass-offsets", "passed": False})
        self.assertEqual(report["lost_pass_offsets"], [-2, -1, 2])
        self.assertIn(0, report["candidate"]["pass_offsets"])

    def test_release_gate_passes_when_all_passing_offsets_are_preserved(self):
        result = self.invoke("before", "--candidate", self.root / "same",
                             "--require-preserved-offsets")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["gate"],
                         {"policy": "preserve-pass-offsets", "passed": True})

    def test_release_gate_rejects_a_missing_candidate(self):
        result = self.invoke("before", "--require-preserved-offsets")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("requires --candidate", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_release_gate_cannot_use_a_missed_reference_as_proof(self):
        result = self.invoke("always-miss", "--candidate", self.root / "same",
                             "--require-preserved-offsets")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("original route does not pass", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_release_gate_rejects_a_sweep_that_tests_only_the_original_offset(self):
        route = json.loads(self.route.read_text())
        route["frames"] = 7
        route["events"].extend([dict(key="Space", frame=3, value=0),
                                dict(key="Space", frame=2, value=1)])
        route["events"].sort(key=lambda event: event["frame"])
        self.route.write_text(json.dumps(route), encoding="utf-8")
        result = self.invoke("before", "--candidate", self.root / "same",
                             "--require-preserved-offsets")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("no valid shifted offsets", result.stderr)

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
