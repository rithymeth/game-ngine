#!/usr/bin/env python3
"""Smoke the distributed command-line tools, without using a source checkout."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def run(tool, *args, cwd, capture=False, env=None):
    print(f"{tool.name}: {' '.join(map(str, args))}", flush=True)
    result = subprocess.run([tool, *map(str, args)], cwd=cwd, env=env, capture_output=capture, text=capture, timeout=90)
    if capture:
        print(result.stdout, end="", flush=True)
        print(result.stderr, end="", flush=True)
    result.check_returncode()
    return (result.stdout or "") + (result.stderr or "")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tools_dir", nargs="?", type=Path, help="Directory containing the distributed tools and player")
    parser.add_argument("--headless-build-tools", nargs=6, type=Path, metavar="EXE",
                        help="Stage the six executables from a build with no window backend")
    parser.add_argument("--template", choices=("blank", "platformer_2d"), default="blank")
    args = parser.parse_args()
    if bool(args.tools_dir) == bool(args.headless_build_tools):
        parser.error("pass either tools_dir or --headless-build-tools")
    exe = ".exe" if os.name == "nt" else ""

    with tempfile.TemporaryDirectory(prefix="Aether project smoke ") as temp:
        root = Path(temp)
        tools_dir = args.tools_dir.resolve() if args.tools_dir else root / "Tools"
        if args.headless_build_tools:
            tools_dir.mkdir()
            for tool in args.headless_build_tools:
                shutil.copy2(tool, tools_dir)
        tools = {name: tools_dir / (name + exe) for name in (
            "aether_new", "aether_loc", "aether_package", "aether_cook", "aether_pak", "aether_player"
        )}
        for tool in tools.values():
            if not tool.is_file():
                parser.error(f"distributed tool is missing: {tool}")

        name = "StarterSmoke"
        source = root / name
        project = source / f"{name}.aproject"
        packaged = root / "Packaged Game"
        empty_cwd = root / "Clean Launch"
        empty_cwd.mkdir()

        run(tools["aether_new"], name, "--template", args.template, "--dir", root, cwd=root)
        run(tools["aether_loc"], project, "--check", cwd=root)
        package_temp = root / "Package Temp"
        existing_scratch = package_temp / "aether-package-smoke"
        existing_scratch.mkdir(parents=True)
        occupied_slot = package_temp / "aether-package-smoke-0"
        occupied_slot.mkdir()
        sentinel = existing_scratch / "untouched"
        sentinel.write_text("owned by someone else", encoding="utf-8")
        (occupied_slot / "untouched").write_text("another smoke run", encoding="utf-8")
        package_env = dict(os.environ, TEMP=str(package_temp), TMP=str(package_temp), TMPDIR=str(package_temp))
        run(tools["aether_package"], project.relative_to(root), "--out", packaged.relative_to(root),
            "--config", "development", "--smoke", cwd=root, env=package_env)
        if (not sentinel.is_file() or not (occupied_slot / "untouched").is_file()
                or set(package_temp.iterdir()) != {existing_scratch, occupied_slot}):
            raise RuntimeError("package smoke deleted existing files or left temporary files behind")
        for required in (packaged / "PackageManifest.json", packaged / "Paks" / "Game.apak"):
            if not required.is_file():
                raise RuntimeError(f"package is missing {required}")
        run(tools["aether_pak"], "verify-package", packaged, cwd=root)

        # The game must discover its own data, with no source tree or working-directory hint.
        source.rename(root / "Hidden Project")
        game = packaged / (name + exe)
        if args.headless_build_tools:
            env = os.environ.copy()
            env.pop("AETHER_PLAYER_HEADLESS", None)
            no_window = subprocess.run([game, "--frames", "1"], cwd=empty_cwd, env=env,
                                       capture_output=True, text=True, timeout=10)
            if no_window.returncode != 2 or "only supports --headless" not in no_window.stderr:
                raise RuntimeError(f"headless-only player did not reject a windowed launch: {no_window.stderr}")
        output = run(game, "--headless", "--frames", "60", "--report",
                     "--user-dir", root / "Player Data", cwd=empty_cwd, capture=True)
        if "Ran 60 frames (" not in output:
            raise RuntimeError("the packaged player did not report 60 frames")
        if args.template == "platformer_2d":
            moved = run(game, "--headless", "--frames", "60", "--press", "Right", "--report",
                        "--user-dir", root / "Player Data With Input", cwd=empty_cwd, capture=True)
            position = r"^Player at \((-?\d+\.\d+),"
            idle_x = re.search(position, output, re.MULTILINE)
            moved_x = re.search(position, moved, re.MULTILINE)
            if ("Ran 60 frames (" not in moved or not idle_x or not moved_x
                    or float(moved_x[1]) <= float(idle_x[1]) + 0.1):
                raise RuntimeError("the packaged platformer did not respond to Right input")


if __name__ == "__main__":
    main()
