"""Test the offline timing-map CLI without requiring an engine build."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

TOOL = Path(__file__).resolve().parents[1] / "jump_envelope_map.py"
SVG = "{http://www.w3.org/2000/svg}"


class JumpEnvelopeMapTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.receipt = self.root / "receipt.json"
        self.report = {
            "schema": "aether.jump-envelope.experimental.v1",
            "scene": '<script>alert("x")</script>', "key": "Space",
            "radius": 2, "fixed_hz": 60, "skipped_offsets": [2],
            "baseline": {"trials": [{"offset": n, "status": s} for n, s in
                                    [(-2, "pass"), (-1, "pass"), (0, "pass"), (1, "missed")]]},
            "candidate": {"trials": [{"offset": n, "status": s} for n, s in
                                     [(-2, "missed"), (-1, "pass"), (0, "pass"), (1, "pass")]]},
            "evidence": {"nearest_lost_replay": {"offset": -2, "divergence": {"first_frame": 3}}}
        }

    def invoke(self, *args):
        self.receipt.write_text(json.dumps(self.report), encoding="utf-8")
        return subprocess.run([sys.executable, TOOL, self.receipt, *args], cwd=self.root,
                              text=True, capture_output=True, timeout=10)

    def test_map_keeps_skip_loss_gain_and_still_passing_original_distinct(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        root = ET.fromstring(result.stdout)
        rows = {int(row.attrib["data-offset"]): row.attrib["data-result"]
                for row in root.findall(f".//{SVG}g[@data-offset]")}
        self.assertEqual(rows, {-2: "lost", -1: "kept", 0: "kept", 1: "gained", 2: "skipped"})
        words = "".join(root.itertext())
        self.assertIn("First split: frame 3 (offset -2)", words)
        self.assertIn("Original timing still passes", words)
        self.assertIn('<script>alert("x")</script>', words)
        self.assertIsNone(root.find(f".//{SVG}script"))

    def test_rejects_missing_coverage_without_writing_a_map(self):
        self.report["candidate"]["trials"].pop()
        dest = self.root / "timing.svg"
        result = self.invoke("--output", dest)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("does not match the tested offsets", result.stderr)
        self.assertFalse(dest.exists())

    def test_tilemap_extract_rejects_missing_paths_and_out_of_range_points(self):
        self.assertIn("no lost-timing Tilemap overlay", self.invoke("--tilemap-overlay").stderr)
        replay = self.report["evidence"]["nearest_lost_replay"]
        replay["tilemap_overlay"] = {
            "frames": 1, "first_difference_frame": 1, "before_status": "pass", "after_status": "missed",
            "verdict_changed": True, "before_path": [[0, 0, 0]], "after_path": [[1000001, 0, 0]],
        }
        dest = self.root / "bad-overlay.json"
        result = self.invoke("--tilemap-overlay", "--output", dest)
        self.assertEqual(result.returncode, 2)
        self.assertIn("outside the Tilemap overlay range", result.stderr)
        self.assertFalse(dest.exists())

    def test_single_build_works_and_preserves_existing_output(self):
        del self.report["candidate"]
        self.assertEqual(self.invoke().returncode, 0)
        dest = self.root / "timing.svg"
        dest.write_text("keep")
        result = self.invoke("--output", dest)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(dest.read_text(), "keep")


if __name__ == "__main__":
    unittest.main()
