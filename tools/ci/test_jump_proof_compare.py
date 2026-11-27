#!/usr/bin/env python3
"""Exercise the Jump Proof comparator without building the player."""

import codecs
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPARE = Path(__file__).resolve().parents[1] / "jump_proof_compare.py"
ROUTE = {"frames": 1, "fixed_hz": 60, "scene": "Level.ascene",
         "target": {"min_x": 0, "max_x": 1, "min_y": 0, "max_y": 1},
         "events": []}


class JumpProofCompareTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="aether-compare-test-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def trace(self, name, position, status, *, descriptor=True, utf16=False):
        path = self.root / name
        lines = ["Jump Route: " + json.dumps(ROUTE)] if descriptor else []
        lines += ["Jump Trace: " + json.dumps({"frame": 1, "position": position}),
                  "Jump Proof: " + json.dumps({"frames": 1, "position": position, "status": status})]
        if utf16:
            path.write_bytes(codecs.BOM_UTF16_LE + ("\r\n".join(lines) + "\r\n").encode("utf-16-le"))
        else:
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path

    def compare(self, before, after, code, *extra):
        result = subprocess.run([sys.executable, COMPARE, before, after, *extra],
                                cwd=self.root, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        return result

    def test_target_edges_pass_and_ignore_z(self):
        left = self.trace("left.txt", [0, 1, 99], "pass")
        right = self.trace("right.txt", [0, 1, -99], "pass", utf16=True)
        receipt = json.loads(self.compare(left, right, 0).stdout)
        self.assertTrue(receipt["route_verified"])
        self.assertIsNone(receipt["first_difference_frame"])

    def test_genuine_miss_is_comparable(self):
        for position in ([2, 0, 0], [0, 2, 0]):
            with self.subTest(position=position):
                trace = self.trace("miss.txt", position, "missed")
                receipt = json.loads(self.compare(trace, trace, 0).stdout)
                self.assertEqual(receipt["before_status"], "missed")
                self.assertFalse(receipt["verdict_changed"])

    def test_inconsistent_verdict_does_not_write_overlay(self):
        cases = (([2, 0, 0], "pass"), ([0, 2, 0], "pass"), ([1, 1, 0], "missed"))
        for i, (position, status) in enumerate(cases):
            with self.subTest(position=position, status=status):
                trace = self.trace("invalid.txt", position, status, utf16=True)
                output = self.root / f"overlay-{i}.json"
                result = self.compare(trace, trace, 2, "--overlay-data", "--output", output)
                self.assertIn("verdict disagrees with the route target", result.stderr)
                self.assertFalse(output.exists())

    def test_outcome_change_survives_large_position_tolerance(self):
        left = self.trace("pass.txt", [1, 1, 0], "pass")
        right = self.trace("miss.txt", [2, 1, 0], "missed")
        receipt = json.loads(self.compare(left, right, 5, "--tolerance", "1000").stdout)
        self.assertTrue(receipt["verdict_changed"])
        self.assertIsNone(receipt["first_difference_frame"])

    def test_legacy_trace_remains_unverified(self):
        trace = self.trace("legacy.txt", [2, 0, 0], "pass", descriptor=False)
        receipt = json.loads(self.compare(trace, trace, 0).stdout)
        self.assertFalse(receipt["route_verified"])


if __name__ == "__main__":
    unittest.main()
