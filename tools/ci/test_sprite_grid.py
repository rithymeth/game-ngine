#!/usr/bin/env python3
"""Create and package a real project with a generated atlas."""
import binascii
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def tool(exe, *args, cwd, expected=0, reason=""):
    result = subprocess.run([exe, *map(str, args)], cwd=cwd, capture_output=True,
                            text=True, timeout=60)
    if result.returncode != expected or reason not in result.stderr:
        raise RuntimeError(f"{exe} {args}: expected {expected} {reason!r}, "
                           f"got {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def png(path, width=12, height=8):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", binascii.crc32(tag + data)))
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress((b"\0" + b"\xff" * width * 4) * height))
                     + chunk(b"IEND", b""))


def main():
    new, grid, package, pak = (Path(s).resolve() for s in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="Aether Grid Smoke ") as tmp:
        root = Path(tmp)
        project = root / "Atlas Lab" / "Atlas Lab.aproject"
        tool(new, "Atlas Lab", "--template", "blank", cwd=root)
        sprites = project.parent / "Content" / "Sprite Sheets"
        sprites.mkdir()
        sheet = sprites / "Hero.png"
        atlas = sheet.with_suffix(".aatlas")
        png(sheet)
        png(root / "Outside.png")
        png(sprites / "Wrong.hdr")
        tool(grid, project, "Sprite Sheets/Wrong.hdr", 4, 4, cwd=root, expected=1,
             reason="supported sprite-sheet texture")
        tool(grid, project, "../../Outside.png", 4, 4, cwd=root, expected=1,
             reason="inside Content")
        tool(grid, project, "Sprite Sheets/Hero.png", 5, 4, cwd=root, expected=1,
             reason="divide")
        if atlas.exists():
            raise RuntimeError("a rejected grid wrote an atlas")
        tool(grid, project.relative_to(root), "Sprite Sheets/Hero.png", 4, 4, cwd=root)
        original = atlas.read_bytes()
        data = json.loads(original)
        meta = json.loads(sheet.with_suffix(".png.ameta").read_bytes())
        if data["texture"] != meta["guid"] or (data["width"], data["height"]) != (12, 8):
            raise RuntimeError("atlas did not reference the imported sheet and its real dimensions")
        frames = [f"frame_{i}" for i in range(6)]
        expected_rects = [(name, i % 3 * 4, i // 3 * 4, 4, 4) for i, name in enumerate(frames)]
        if [tuple(frame[k] for k in ("name", "x", "y", "w", "h"))
                for frame in data["frames"]] != expected_rects:
            raise RuntimeError("grid did not slice the sheet in reading order")
        if data["clips"] != [{"name": "grid", "frames": frames, "fps": 12.0, "loop": True}]:
            raise RuntimeError("grid was not saved as an animation clip")
        tool(grid, project, "Sprite Sheets/Hero.png", 3, 4, cwd=root, expected=1,
             reason="already exists")
        if atlas.read_bytes() != original:
            raise RuntimeError("grid overwrote existing work")
        second_sheet = sprites / "Motion.png"
        second_atlas = second_sheet.with_suffix(".aatlas")
        png(second_sheet)
        args = (project, "Sprite Sheets/Motion.png", 4, 4)
        long_but_valid = "land_" + "a" * 26
        clips = ("--clip", "idle", 0, 2, 6, "--clip", "run", 3, 3, 18,
                 "--clip-once", long_but_valid, 2, 2, 12)
        tool(grid, *args, "--clip", long_but_valid + "a", 0, 1, 8, cwd=root, expected=2,
             reason="1 to 31")
        tool(grid, *args, "--clip-once", "jump", 0, 2, 10, "--clip", "jump", 5, 1, 12,
             cwd=root, expected=2, reason="duplicate clip name")
        tool(grid, *args, *clips, "--clip", "idle", 1, 1, 8, cwd=root, expected=2,
             reason="duplicate clip name")
        for first, count in ((5, 2), (6, 1), (4294967295, 2)):
            tool(grid, *args, "--clip", "run", first, count, 18, cwd=root, expected=1,
                 reason="outside the 6-frame grid")
        for frames_per_second in (0, 241, "bad"):
            tool(grid, *args, "--clip", "run", 0, 1, frames_per_second, cwd=root, expected=2,
                 reason="FPS must be")
        tool(grid, *args, "--clip", "run", 0, 0, 12, cwd=root, expected=2,
             reason="positive COUNT")
        if second_atlas.exists() or second_sheet.with_suffix(".png.ameta").exists():
            raise RuntimeError("an invalid clip wrote an atlas or imported its sheet")
        tool(grid, *args, *clips, cwd=root)
        named = json.loads(second_atlas.read_bytes())
        expected_clips = [{"name": "idle", "frames": frames[:2], "fps": 6, "loop": True},
                          {"name": "run", "frames": frames[3:], "fps": 18, "loop": True},
                          {"name": long_but_valid, "frames": frames[2:4], "fps": 12, "loop": False}]
        if named["frames"] != data["frames"] or named["clips"] != expected_clips:
            raise RuntimeError("named clips did not preserve sheet frames or selected ranges")
        out = root / "Packaged Atlas"
        tool(package, project, "--out", out, "--config", "shipping", "--strict", "--always",
             "Sprite Sheets/Motion.aatlas", cwd=root)
        listing = tool(pak, "list", out / "Paks" / "Game.apak", cwd=root)
        if any(f"Content/Sprite Sheets/Motion.{ext}" not in listing for ext in ("aatlas", "png")):
            raise RuntimeError("the atlas alone did not pull its source texture into Shipping")
        extract = root / "Extracted Atlas"
        tool(pak, "extract", out / "Paks" / "Game.apak", extract, cwd=root)
        packed = json.loads((extract / "Content" / "Sprite Sheets" / "Motion.aatlas").read_bytes())
        if packed != named:
            raise RuntimeError("Shipping did not preserve the authored named clips")
        padded_sheet = sprites / "Padded.png"
        padded_atlas = padded_sheet.with_suffix(".aatlas")
        png(padded_sheet, 16, 13)  # 3x2 cells, 1x2 border, 1x1 internal gaps
        padded_args = (project, "Sprite Sheets/Padded.png", 4, 4)
        for margin in ((1, 1), (4294967295, 2)):
            tool(grid, *padded_args, "--margin", *margin, "--spacing", 1, 1, cwd=root, expected=1,
                 reason="complete cells")
        tool(grid, *padded_args, "--margin", 1, 2, "--margin", 1, 2, cwd=root, expected=2,
             reason="only once")
        if padded_atlas.exists():
            raise RuntimeError("invalid padding wrote an atlas")
        tool(grid, *padded_args, "--spacing", 1, 1, "--clip", "row2", 3, 3, 8, "--margin", 1, 2, cwd=root)
        padded = json.loads(padded_atlas.read_bytes())
        rects = [(1 + i%3*5, 2 + i//3*5, 4, 4) for i in range(6)]
        if ((padded["width"],padded["height"]) != (16,13)
                or [tuple(f[k] for k in ("x","y","w","h")) for f in padded["frames"]] != rects
                or padded["clips"][0]["frames"] != frames[3:]):
            raise RuntimeError("authored clip did not follow the padded grid")
        padded_out = root / "Packaged Padded Atlas"
        tool(package, project, "--out", padded_out, "--config", "shipping", "--strict", "--always",
             "Sprite Sheets/Padded.aatlas", cwd=root)
        padded_extract = root / "Extracted Padded Atlas"
        tool(pak, "extract", padded_out / "Paks" / "Game.apak", padded_extract, cwd=root)
        packed_dir = padded_extract / "Content" / "Sprite Sheets"
        if json.loads((packed_dir / "Padded.aatlas").read_bytes()) != padded or not (packed_dir / "Padded.png").exists():
            raise RuntimeError("Shipping did not preserve padded coordinates and sheet")

        missing_texture = "5a6d6c84-f7c2-4d70-ab39-6710ff83e948"
        broken = dict(named, texture=missing_texture)
        (sprites / "MissingArt.aatlas").write_text(json.dumps(broken), encoding="utf-8")
        broken_output = root / "Broken Atlas Distribution"
        tool(package, project, "--out", broken_output, "--config", "shipping", "--strict",
             "--always", "Sprite Sheets/MissingArt.aatlas", cwd=root, expected=1,
             reason=f"Sprite Sheets/MissingArt.aatlas refers to a missing asset {missing_texture}")
        if (broken_output / "Paks" / "Game.apak").exists():
            raise RuntimeError("strict Shipping emitted a package for an unknown texture GUID")


if __name__ == "__main__":
    main()
