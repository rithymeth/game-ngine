#!/usr/bin/env python3
"""Exercise the Jump Proof tools without building the player."""

import argparse
import codecs
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import jump_proof_isolate_tile as isolator

COMPARE = TOOLS / "jump_proof_compare.py"
ROUTE = {"frames": 1, "fixed_hz": 60, "scene": "Level.ascene",
         "target": {"min_x": 0, "max_x": 1, "min_y": 0, "max_y": 1},
         "events": []}


class JumpProofCompareTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="aether-compare-test-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()

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


class JumpProofIsolateTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="aether-isolate-test-")
        self.addCleanup(directory.cleanup)
        root = Path(directory.name).resolve()
        self.args = argparse.Namespace(before=root / "before", after=root / "after",
                                       pak_tool=root / "aether_pak", player="MyGame.exe",
                                       route=root / "route.json", overlay_data=True)
        self.args.pak_tool.touch()
        self.args.route.write_text(json.dumps(ROUTE), encoding="utf-8")
        for package in (self.args.before, self.args.after):
            package.mkdir()
            (package / self.args.player).write_bytes(b"same player")

    @staticmethod
    def trace(position, status, route=ROUTE):
        records = [("Jump Route: ", route)] if route is not None else []
        records += [("Jump Trace: ", {"frame": 1, "position": position}),
                    ("Jump Proof: ", {"frames": 1, "position": position, "status": status})]
        return "".join(prefix + json.dumps(record) + "\n" for prefix, record in records)

    def isolate(self, *, after_route=ROUTE, repeat_route=ROUTE, after_status="missed", after_x=2):
        traces = iter(((0, self.trace([1, 1, 0], "pass")),
                       (5, self.trace([after_x, 1, 0], after_status, after_route)),
                       (0, self.trace([1, 1, 0], "pass", repeat_route))))

        def run(command, **kwargs):
            exe, action, *rest = command
            if exe == self.args.pak_tool:
                if action == "extract":
                    archive, destination = rest
                    asset = destination / "Content" / "Level.atilemap"
                    asset.parent.mkdir(parents=True)
                    cell = 1 if archive.parent.parent == self.args.before else 2
                    asset.write_text(json.dumps({"$type": "Tilemap", "size": [1, 1],
                                                 "layers": [{"cells": [cell]}]}), encoding="utf-8")
                return subprocess.CompletedProcess(command, 0, "", "")
            code, stdout = next(traces)
            return subprocess.CompletedProcess(command, code, stdout, "")

        with patch.object(isolator.subprocess, "run", side_effect=run):
            return isolator.isolate(self.args)

    def test_matching_route_preserves_verdicts_in_overlay(self):
        receipt = self.isolate()
        self.assertEqual(receipt["cell"], {"layer": 0, "x": 0, "y": 0, "before": 1, "after": 2})
        self.assertEqual(receipt["first_difference_frame"], 1)
        self.assertEqual(receipt["before_status"], "pass")
        self.assertEqual(receipt["after_status"], "missed")
        self.assertTrue(receipt["verdict_changed"])

    def test_target_edge_keeps_single_tile_evidence_without_a_position_marker(self):
        # X=1 is on the inclusive target boundary. A smaller-than-tolerance
        # step outside changes the verdict without making a path marker.
        receipt = self.isolate(after_x=1.002)
        self.assertEqual(receipt["asset"], "Content/Level.atilemap")
        self.assertEqual(receipt["cell"], {"layer": 0, "x": 0, "y": 0, "before": 1, "after": 2})
        self.assertEqual((receipt["before_status"], receipt["after_status"]), ("pass", "missed"))
        self.assertTrue(receipt["verdict_changed"])
        self.assertIsNone(receipt["first_difference_frame"])
        self.assertNotIn("before_position", receipt)
        self.assertNotIn("after_position", receipt)
        self.assertEqual(receipt["before_path"], [[1, 1, 0]])
        self.assertEqual(receipt["after_path"], [[1.002, 1, 0]])

    def test_different_route_rejected_in_changed_build_and_repeat(self):
        different = copy.deepcopy(ROUTE)
        different["fixed_hz"] = 120
        for argument in ("after_route", "repeat_route"):
            with self.subTest(argument=argument):
                with self.assertRaisesRegex(ValueError, "route descriptors differ"):
                    self.isolate(**{argument: different})

    def test_missing_descriptor_is_not_controlled_evidence(self):
        with self.assertRaisesRegex(ValueError, "missing route descriptor"):
            self.isolate(after_route=None)

    def test_trace_verdict_must_agree_with_player_exit_code(self):
        # The modified trace's pass verdict is consistent with its target, but not its exit code.
        with self.assertRaisesRegex(ValueError, "verdict disagrees with the player exit code"):
            self.isolate(after_status="pass", after_x=0)


if __name__ == "__main__":
    unittest.main()
