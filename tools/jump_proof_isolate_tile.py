#!/usr/bin/env python3
"""Check which single tile differs between two controlled Jump Proof builds."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from jump_proof_compare import read_trace

IGNORED_PACKAGE_FILES = {"PackageManifest.json", "Paks/Game.apak"}
MAP_LIMIT = 64 * 1024 * 1024
TOLERANCE = 0.01


def file_digests(root, excluded=()):
    result = {}
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError("symbolic links are unsupported in controlled builds")
        name = path.relative_to(root).as_posix()
        if not path.is_file() or name in excluded:
            continue
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        result[name] = digest.digest()
    return result


def run(args, folder, valid=(0,)):
    proc = subprocess.run(args, cwd=folder, capture_output=True, text=True, timeout=60, check=False)
    if proc.returncode not in valid:
        raise ValueError(f"{Path(args[0]).name} exited with {proc.returncode}: {proc.stderr.strip()}")
    return proc


def read_map(path):
    if path.stat().st_size > MAP_LIMIT:
        raise ValueError("tilemap is too large for single-tile attribution")
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("$type") != "Tilemap":
        raise ValueError("expected Aether Tilemap JSON")
    return data


def changed_cell(left, right):
    def metadata(data, except_key):
        return {k: v for k, v in data.items() if k != except_key}

    if metadata(left, "layers") != metadata(right, "layers"):
        raise ValueError("tilemap metadata changed; cannot isolate a cell")
    size = left.get("size")
    if (not isinstance(size, list) or len(size) != 2
            or any(type(n) is not int or n <= 0 for n in size) or size[0] * size[1] > 4_194_304):
        raise ValueError("invalid tilemap dimensions")
    before, after = left.get("layers"), right.get("layers")
    if not isinstance(before, list) or not isinstance(after, list) or len(before) != len(after):
        raise ValueError("tilemap layers changed; cannot isolate a cell")
    result = None
    for idx, (a, b) in enumerate(zip(before, after)):
        if not isinstance(a, dict) or not isinstance(b, dict) or metadata(a, "cells") != metadata(b, "cells"):
            raise ValueError("tilemap layer settings changed; cannot isolate a cell")
        ac, bc = a.get("cells"), b.get("cells")
        if not isinstance(ac, list) or not isinstance(bc, list) or len(ac) != size[0] * size[1] or len(ac) != len(bc):
            raise ValueError("invalid tilemap cell count")
        for i, (old, new) in enumerate(zip(ac, bc)):
            if type(old) is not int or type(new) is not int:
                raise ValueError("invalid tilemap cell")
            if old == new:
                continue
            if result is not None:
                raise ValueError("more than one tile changed")
            result = {"layer": idx, "x": i % size[0], "y": i // size[0], "before": old, "after": new}
    if result is None:
        raise ValueError("no tile changed")
    return result


def isolate(args):
    original, modified = args.before.resolve(strict=True), args.after.resolve(strict=True)
    tool = args.pak_tool.resolve(strict=True)
    if (Path(args.player).name != args.player or not args.player
            or "/" in args.player or "\\" in args.player or ":" in args.player
            or args.player in (".", "..")):
        raise ValueError("--player must be a filename within both packages")
    with tempfile.TemporaryDirectory(prefix="aether-jump-proof-") as scratch:
        root = Path(scratch)
        route = root / "route.json"
        route.write_bytes(args.route.read_bytes())
        for package in (original, modified):
            run([tool, "verify-package", package], root)
        original_files = file_digests(original, IGNORED_PACKAGE_FILES)
        if args.player not in original_files or original_files != file_digests(modified, IGNORED_PACKAGE_FILES):
            raise ValueError("files outside Game.apak differ or the shared player is missing")
        before_extract, after_extract = root / "before-content", root / "after-content"
        contents = []
        for package, extracted in ((original, before_extract), (modified, after_extract)):
            run([tool, "extract", package / "Paks/Game.apak", extracted], root)
            contents.append(file_digests(extracted))
        if contents[0].keys() != contents[1].keys():
            raise ValueError("archive contains added or removed entries")
        changed = [name for name, digest in contents[0].items() if digest != contents[1][name]]
        if len(changed) != 1 or not changed[0].startswith("Content/") or not changed[0].endswith(".atilemap"):
            raise ValueError("Game.apak must differ in exactly one tilemap asset")
        asset = changed[0]
        cell = changed_cell(read_map(before_extract / asset), read_map(after_extract / asset))

        traces = []
        for index, (package, expectation) in enumerate(((original, 0), (modified, 5), (original, 0))):
            proc = run([package / args.player, "--jump-proof", route, "--jump-trace",
                        "--user-dir", root / f"profile-{index}"], root, valid=(0, 5))
            if proc.returncode != expectation:
                raise ValueError("controlled replay must pass before, miss after, and pass on a repeated baseline")
            trace = root / f"trace-{index}.txt"
            trace.write_text(proc.stdout, encoding="utf-8")
            traces.append(read_trace(trace))
        before, after, repeat = traces
        if before != repeat or len(before) != len(after):
            raise ValueError("baseline was not repeatable or trace lengths differ")
        for frame, (a, b) in enumerate(zip(before, after), 1):
            if any(abs(a[i] - b[i]) > TOLERANCE for i in (0, 1)):
                return {"asset": asset, "cell": cell, "frames": len(before),
                        "first_difference_frame": frame, "before_position": a, "after_position": b}
        raise ValueError("no position difference exceeds the trace tolerance")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pak-tool", type=Path, required=True)
    parser.add_argument("--player", required=True, help="shared executable filename, including .exe on Windows")
    parser.add_argument("--route", type=Path, required=True)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(isolate(args), allow_nan=False))
    except (ValueError, OSError, OverflowError, subprocess.TimeoutExpired) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
