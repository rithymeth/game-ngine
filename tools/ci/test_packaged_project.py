#!/usr/bin/env python3
"""Smoke the distributed command-line tools, without using a source checkout."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run(tool, *args, cwd):
    print(f"{tool.name}: {' '.join(map(str, args))}", flush=True)
    subprocess.run([tool, *map(str, args)], cwd=cwd, check=True, timeout=90)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tools_dir", type=Path, help="Directory containing the distributed tools and player")
    args = parser.parse_args()
    tools_dir = args.tools_dir.resolve()
    exe = ".exe" if os.name == "nt" else ""
    tools = {name: tools_dir / (name + exe) for name in (
        "aether_new", "aether_loc", "aether_package", "aether_cook", "aether_pak", "aether_player"
    )}
    for tool in tools.values():
        if not tool.is_file():
            parser.error(f"distributed tool is missing: {tool}")

    with tempfile.TemporaryDirectory(prefix="Aether project smoke ") as temp:
        root = Path(temp)
        name = "StarterSmoke"
        source = root / name
        project = source / f"{name}.aproject"
        packaged = root / "Packaged Game"
        empty_cwd = root / "Clean Launch"
        empty_cwd.mkdir()

        run(tools["aether_new"], name, "--template", "blank", "--dir", root, cwd=root)
        run(tools["aether_loc"], project, "--check", cwd=root)
        run(tools["aether_package"], project, "--out", packaged, "--config", "development", cwd=root)
        for required in (packaged / "PackageManifest.json", packaged / "Paks" / "Game.apak"):
            if not required.is_file():
                raise RuntimeError(f"package is missing {required}")

        # The game must discover its own data, with no source tree or working-directory hint.
        source.rename(root / "Hidden Project")
        run(packaged / (name + exe), "--headless", "--frames", "60", "--report",
            "--user-dir", root / "Player Data", cwd=empty_cwd)


if __name__ == "__main__":
    main()
