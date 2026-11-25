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


def png(path):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", binascii.crc32(tag + data)))
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", 12, 8, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress((b"\0" + b"\xff" * 48) * 8))
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
        out = root / "Packaged Atlas"
        tool(package, project, "--out", out, "--config", "shipping", "--strict", "--always",
             "Sprite Sheets/Hero.aatlas", cwd=root)
        listing = tool(pak, "list", out / "Paks" / "Game.apak", cwd=root)
        if any(f"Content/Sprite Sheets/Hero.{ext}" not in listing for ext in ("aatlas", "png")):
            raise RuntimeError("the atlas alone did not pull its source texture into Shipping")


if __name__ == "__main__":
    main()
