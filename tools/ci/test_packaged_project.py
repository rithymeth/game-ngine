#!/usr/bin/env python3
"""Smoke the distributed command-line tools, without using a source checkout."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

MOTION = {
    "platformer_2d": ("Right", 0, 1),
    "first_person": ("W", 2, -1),
    "third_person": ("W", 2, -1),
    "top_down": ("W", 2, -1),
    "vehicle": ("W", 2, -1),
}
MODELS = {
    "first_person": ("Floor", "Pickup"),  # the player is the camera, with no body mesh
    "third_person": ("Floor", "Player", "Pickup"),
    "top_down": ("Floor", "Player", "Pickup"),
    "vehicle": ("Floor", "Player", "Pickup"),
}
COORD = r"(-?\d+\.\d+)"
PLAYER_POSITION = re.compile(rf"^Player at \({COORD}, {COORD}, {COORD}\)", re.MULTILINE)
SIXTY_FRAMES = re.compile(r"^Simulation frames: 60$", re.MULTILINE)


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
    parser.add_argument("--template", choices=("blank", *MOTION), default="blank")
    parser.add_argument("--editor-runner", type=Path, help="Also exercise the editor's packaging command layer")
    args = parser.parse_args()
    if bool(args.tools_dir) == bool(args.headless_build_tools):
        parser.error("pass either tools_dir or --headless-build-tools")
    exe = ".exe" if os.name == "nt" else ""

    with tempfile.TemporaryDirectory(prefix="Aether owner's smoke ") as temp:
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

        for invalid_name in ("cOn", "PrN", "AUX", "NUL", "COM0", "COM1", "lpt9", "C O M 2"):
            rejected = subprocess.run([tools["aether_new"], invalid_name, "--dir", root],
                                      cwd=root, capture_output=True, text=True, timeout=30)
            if rejected.returncode == 0 or "reserved Windows name" not in rejected.stderr:
                raise RuntimeError(f"Scaffold did not explain the reserved name {invalid_name!r}")
            if invalid_name in {path.name for path in root.iterdir()}:
                raise RuntimeError(f"Scaffold left a partial project for {invalid_name!r}")

        name = "Starter Smoke"
        executable = "StarterSmoke" + exe
        source = root / name
        project = source / f"{name}.aproject"
        packaged = root / "Packaged Game"
        empty_cwd = root / "Clean Launch"
        empty_cwd.mkdir()

        scaffold = run(tools["aether_new"], name, "--template", args.template, "--dir", root, cwd=root, capture=True)
        package_line, launch_line = (line.strip() for line in scaffold.strip().splitlines()[-2:])
        expected_build = str(source / "Build")
        expected_player = str(source / "Build" / executable)
        if os.name == "nt":
            quote = lambda path: "'" + path.replace("'", "''") + "'"
            matching = (launch_line == "& " + quote(expected_player)
                        and package_line == f"aether_package {quote(str(project))} --out {quote(expected_build)}")
        else:
            matching = (shlex.split(launch_line) == [expected_player]
                        and shlex.split(package_line) == ["aether_package", str(project), "--out", expected_build])
        if not matching:
            raise RuntimeError("aether_new did not print shell-ready commands for the packaged player:\n"
                               f"{package_line}\n{launch_line}")
        run(tools["aether_loc"], project, "--check", cwd=root)
        if args.editor_runner:
            editor_env = dict(os.environ, AETHER_USER_DIR=str(root / "Editor Player Data"))
            editor_output = run(args.editor_runner.resolve(), project.relative_to(root), tools["aether_player"],
                                cwd=root, env=editor_env, capture=True)
            if not SIXTY_FRAMES.search(editor_output):
                raise RuntimeError("the editor's Launch command did not run 60 frames")
        package_temp = root / "Package Temp"
        existing_scratch = package_temp / "aether-package-smoke"
        existing_scratch.mkdir(parents=True)
        occupied_slot = package_temp / "aether-package-smoke-0"
        occupied_slot.mkdir()
        sentinel = existing_scratch / "untouched"
        sentinel.write_text("owned by someone else", encoding="utf-8")
        (occupied_slot / "untouched").write_text("another smoke run", encoding="utf-8")
        package_env = dict(os.environ, TEMP=str(package_temp), TMP=str(package_temp), TMPDIR=str(package_temp))
        release_config = ["--config", "shipping", "--strict"] if args.headless_build_tools else ["--config", "development"]
        package_output = run(tools["aether_package"], project.relative_to(root), "--out", packaged.relative_to(root),
                             *release_config, "--smoke", cwd=root, env=package_env, capture=True)
        if args.template in MODELS and "Skipping unknown" in package_output:
            raise RuntimeError("the valid 3D template generated misleading preflight component warnings")
        if (not sentinel.is_file() or not (occupied_slot / "untouched").is_file()
                or set(package_temp.iterdir()) != {existing_scratch, occupied_slot}):
            raise RuntimeError("package smoke deleted existing files or left temporary files behind")
        for required in (packaged / "PackageManifest.json", packaged / "Paks" / "Game.apak"):
            if not required.is_file():
                raise RuntimeError(f"package is missing {required}")
        manifest = json.loads((packaged / "PackageManifest.json").read_text(encoding="utf-8"))
        if manifest.get("configuration") != release_config[1].capitalize():
            raise RuntimeError("package manifest recorded the wrong build configuration")
        run(tools["aether_pak"], "verify-package", packaged, cwd=root)
        if args.template in MODELS:
            listing = run(tools["aether_pak"], "list", packaged / "Paks" / "Game.apak", cwd=root, capture=True)
            for model in MODELS[args.template]:
                if f"Content/Models/{model}.gltf" not in listing:
                    raise RuntimeError(f"the packaged 3D starter is missing {model}.gltf")
        if args.headless_build_tools and args.template == "blank":
            # An extensionless executable cannot share a name with a package folder.
            for project_name, executable_name in (("P a k s", "Paks-game"), ("LICENSES", "LICENSES-game")):
                created = run(tools["aether_new"], project_name, "--dir", root, cwd=root, capture=True)
                collision_source = root / project_name
                launch_command = created.strip().splitlines()[-1]
                if shlex.split(launch_command) != [str(collision_source / "Build" / executable_name)]:
                    raise RuntimeError(f"Scaffold printed the wrong executable for {project_name!r}")
                collision_output = root / (project_name + " Distribution")
                run(tools["aether_package"], collision_source / (project_name + ".aproject"),
                    "--out", collision_output, "--smoke", cwd=root, env=package_env)
                if not (collision_output / executable_name).is_file() or not (collision_output / "Paks").is_dir():
                    raise RuntimeError(f"Package file and data folder collide for {project_name!r}")
                run(tools["aether_pak"], "verify-package", collision_output, cwd=root)
            # Keep the real player busy well past the deadline, without a fake
            # executable or an actual hang. Capture output to check the cause.
            timed_out = subprocess.run(
                [tools["aether_package"], str(project.relative_to(root)), "--out", "Timeout Package",
                 "--config", "development", "--smoke", "--smoke-frames", "2147483647", "--smoke-timeout", "1"],
                cwd=root, env=package_env, capture_output=True, text=True, timeout=12)
            if timed_out.returncode != 1 or "timed out after 1 second" not in timed_out.stderr:
                raise RuntimeError(f"package smoke did not report a timeout: {timed_out.stderr}")
            if set(package_temp.iterdir()) != {existing_scratch, occupied_slot}:
                raise RuntimeError("timed-out smoke left temporary files behind")
            run(tools["aether_pak"], "verify-package", root / "Timeout Package", cwd=root)
            original_project = project.read_bytes()
            previous_manifest = (packaged / "PackageManifest.json").read_bytes()
            previous_pak = (packaged / "Paks" / "Game.apak").read_bytes()
            try:
                for startup in ("", "Scenes/DoesNotExist.ascene"):
                    settings = json.loads(original_project)
                    settings["startup_scene"] = startup
                    settings["always_cook"] = ["Scenes/"]
                    project.write_text(json.dumps(settings), encoding="utf-8")
                    failed = subprocess.run(
                        [tools["aether_package"], str(project), "--out", str(packaged)],
                        cwd=root, capture_output=True, text=True, timeout=12)
                    if failed.returncode != 1 or "startup scene" not in failed.stderr.lower():
                        raise RuntimeError(f"unlaunchable startup scene was not rejected: {startup!r}\n"
                                           f"{failed.stdout}{failed.stderr}")
                    if ((packaged / "PackageManifest.json").read_bytes() != previous_manifest
                            or (packaged / "Paks" / "Game.apak").read_bytes() != previous_pak
                            or any(p.name.startswith(".aether-package-") for p in packaged.iterdir())):
                        raise RuntimeError("a failed package replaced the prior build or left staging files")
            finally:
                project.write_bytes(original_project)
            startup_file = source / "Content" / json.loads(original_project)["startup_scene"]
            original_scene = startup_file.read_bytes()
            try:
                for bad_scene in (b'{"$type":"Scene",', b'{"$type":"Scene","$version":1}',
                                  b'AESC\x02\x00\x00\x00'):
                    startup_file.write_bytes(bad_scene)
                    failed = subprocess.run(
                        [tools["aether_package"], str(project), "--out", str(packaged)],
                        cwd=root, capture_output=True, text=True, timeout=12)
                    if failed.returncode != 1 or "startup scene cannot be loaded" not in failed.stderr.lower():
                        raise RuntimeError("a malformed startup scene was not rejected\n"
                                           f"{failed.stdout}{failed.stderr}")
                    if ((packaged / "PackageManifest.json").read_bytes() != previous_manifest
                            or (packaged / "Paks" / "Game.apak").read_bytes() != previous_pak
                            or any(p.name.startswith(".aether-package-") for p in packaged.iterdir())):
                        raise RuntimeError("a malformed startup scene replaced the prior build or left staging files")
            finally:
                startup_file.write_bytes(original_scene)
            unknown_type = "AetherPackageUnknownTestComponent"
            with_unknown = json.loads(original_scene)
            with_unknown["entities"][0]["components"][unknown_type] = {}
            unknown_package = root / "Unknown Component Package"
            try:
                startup_file.write_text(json.dumps(with_unknown), encoding="utf-8")
                preflight = run(tools["aether_package"], project, "--out", unknown_package,
                                "--config", "development", cwd=root, capture=True)
                if "Skipping unknown" in preflight:
                    raise RuntimeError("preflight diagnosed a component without the runtime registry")
            finally:
                startup_file.write_bytes(original_scene)
            runtime = run(unknown_package / executable, "--headless", "--frames", "1", "--user-dir",
                          root / "Unknown Component Player Data", cwd=empty_cwd, capture=True)
            if not any("Skipping unknown" in line and unknown_type in line for line in runtime.splitlines()):
                raise RuntimeError("the real player did not report its unknown component")
            encrypted = root / "Encrypted Package"
            fixture_key = "12" * 32
            encrypted_smoke = subprocess.run(
                [tools["aether_package"], str(project), "--out", str(encrypted),
                 "--config", "development", "--key", fixture_key, "--smoke"],
                cwd=root, capture_output=True, text=True, timeout=12,
                env=dict(os.environ, AETHER_PAK_KEY="34" * 32))
            smoke_log = encrypted_smoke.stdout + encrypted_smoke.stderr
            if fixture_key in smoke_log:
                raise RuntimeError("encrypted smoke printed its key")
            if encrypted_smoke.returncode != 0 or "smoke test: the packaged game ran" not in smoke_log:
                raise RuntimeError("encrypted package did not smoke with its own key\n" + smoke_log)
            run(encrypted / executable, "--headless", "--frames", "1", "--user-dir",
                root / "Encrypted Player Data", cwd=empty_cwd, env=dict(os.environ, AETHER_PAK_KEY=fixture_key))

        # Simulate moving the distribution after build, with neither the source
        # tree nor the original package location available to the executable.
        source.rename(root / "Hidden Project")
        relocated = root / "Moved Distribution" / "Installed Game"
        relocated.parent.mkdir()
        packaged.rename(relocated)
        game = relocated / executable
        if args.headless_build_tools:
            env = os.environ.copy()
            env.pop("AETHER_PLAYER_HEADLESS", None)
            no_window = subprocess.run([game, "--frames", "1"], cwd=empty_cwd, env=env,
                                       capture_output=True, text=True, timeout=10)
            if no_window.returncode != 2 or "only supports --headless" not in no_window.stderr:
                raise RuntimeError(f"headless-only player did not reject a windowed launch: {no_window.stderr}")
        output = run(game, "--headless", "--frames", "60", "--report",
                     "--user-dir", root / "Player Data", cwd=empty_cwd, capture=True)
        if not SIXTY_FRAMES.search(output):
            raise RuntimeError("the packaged player did not report 60 frames")
        if args.template in MOTION:
            key, axis, direction = MOTION[args.template]
            moved = run(game, "--headless", "--frames", "60", "--press", key, "--report",
                        "--user-dir", root / "Player Data With Input", cwd=empty_cwd, capture=True)
            idle_position = PLAYER_POSITION.search(output)
            moved_position = PLAYER_POSITION.search(moved)
            if (not SIXTY_FRAMES.search(moved) or not idle_position or not moved_position
                    or direction * (float(moved_position[axis + 1]) - float(idle_position[axis + 1])) <= .1):
                raise RuntimeError(f"the packaged {args.template} did not respond to {key} input")


if __name__ == "__main__":
    main()
