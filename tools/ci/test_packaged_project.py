#!/usr/bin/env python3
"""Smoke the distributed command-line tools, without using a source checkout."""

import argparse
import codecs
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
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
            original_package = {p: p.read_bytes() for p in
                                (packaged / "PackageManifest.json", packaged / executable, packaged / "Paks" / "Game.apak")}
            other = "Other Town"
            run(tools["aether_new"], other, "--dir", root, cwd=root)
            rejected = subprocess.run(
                [tools["aether_package"], str(root / other / (other + ".aproject")), "--out", str(packaged)],
                cwd=root, capture_output=True, text=True, timeout=12)
            if rejected.returncode != 1 or "contains a package for another project" not in rejected.stderr:
                raise RuntimeError("the packager did not reject a different project in the same output folder")
            if (any(p.read_bytes() != before for p, before in original_package.items())
                    or any(p.name.startswith(".aether-package-") for p in packaged.iterdir())
                    or (packaged / ("OtherTown" + exe)).exists()):
                raise RuntimeError("rejecting the other project changed the original package")
            other_project = root / other / (other + ".aproject")
            other_config = json.loads(other_project.read_text(encoding="utf-8"))
            other_main = other_project.parent / "Content" / other_config["startup_scene"]
            alternate = other_main.with_name("Alternate.ascene")
            other_main.rename(alternate)
            other_config["startup_scene"] = r"Scenes\Alternate.ascene"
            other_project.write_text(json.dumps(other_config), encoding="utf-8")
            alternate_build = root / "Alternate Scene Build"
            run(tools["aether_package"], other_project, "--out", alternate_build,
                "--config", "shipping", "--strict", "--smoke", cwd=root, env=package_env)
            alternate_listing = run(tools["aether_pak"], "list", alternate_build / "Paks/Game.apak",
                                    cwd=root, capture=True)
            if "Content/Scenes/Alternate.ascene" not in alternate_listing or "Content/Scenes/Main.ascene" in alternate_listing:
                raise RuntimeError("the portable startup path did not select the renamed scene")
            # Explicit assets can also come from Windows-created project settings or CLI arguments.
            for relative, value in (("Notes/Manual.txt", "folder data"), ("Data/Rules.txt", "single-file data")):
                extra = other_project.parent / "Content" / relative
                extra.parent.mkdir(exist_ok=True)
                extra.write_text(value, encoding="utf-8")
            other_config["always_cook"] = ["Notes\\"]
            other_project.write_text(json.dumps(other_config), encoding="utf-8")
            run(tools["aether_package"], other_project, "--out", alternate_build,
                "--always", r"Data\Rules.txt", "--strict", "--smoke", cwd=root, env=package_env)
            portable_entries = run(tools["aether_pak"], "list", alternate_build / "Paks/Game.apak",
                                   cwd=root, capture=True)
            if any(path not in portable_entries for path in ("Content/Notes/Manual.txt", "Content/Data/Rules.txt")):
                raise RuntimeError("the portable path roots did not include both files")
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


        if args.template == "platformer_2d":
            route = root / "Jump Proof route.json"
            spec = {"frames": 400, "events": [
                {"frame": 90, "key": "D", "value": 1}, {"frame": 225, "key": "Space", "value": 1},
                {"frame": 285, "key": "Space", "value": 0}, {"frame": 350, "key": "D", "value": 0},
            ], "target": {"min_x": 28.5, "max_x": 30, "min_y": 2, "max_y": 3}}
            route.write_text(json.dumps(spec), encoding="utf-8")

            def proof(player, expected_code, profile, trace=None):
                extra = ["--jump-trace"] if trace is not None else []
                proc = subprocess.run([player, "--jump-proof", route, "--user-dir", root / profile, *extra],
                                      cwd=empty_cwd, capture_output=True, text=True, timeout=30)
                if trace is not None:
                    trace.write_text(proc.stdout, encoding="utf-8")
                if proc.returncode != expected_code:
                    raise RuntimeError(f"wrong Jump Proof exit code {proc.returncode}:\n{proc.stdout}{proc.stderr}")
                return [json.loads(line.removeprefix("Jump Proof: ")) for line in proc.stdout.splitlines()
                        if line.startswith("Jump Proof: ")]

            recorded = root / "Recorded route.json"
            run(game, "--headless", "--replay-input", route, "--record-jump-route", recorded,
                "--user-dir", root / "Capture Route Data", cwd=empty_cwd)
            captured = json.loads(recorded.read_text(encoding="utf-8"))
            if captured["frames"] != spec["frames"] or captured["events"] != spec["events"]:
                raise RuntimeError("route capture did not preserve the fixed-tick input sequence")
            captured_check = run(game, "--jump-proof", recorded, "--user-dir",
                                 root / "Recorded Check Data", cwd=empty_cwd, capture=True)
            captured_verdict = next(json.loads(line.removeprefix("Jump Proof: "))
                                    for line in captured_check.splitlines() if line.startswith("Jump Proof: "))
            if captured_verdict["status"] != "pass" or abs(captured_verdict["position"][0] - 29) > .02:
                raise RuntimeError(f"recorded route did not replay to the captured landing: {captured_verdict}")
            hidden_project = root / "Hidden Project" / project.name
            project_bytes = hidden_project.read_bytes()
            settings = json.loads(project_bytes)
            startup = settings["startup_scene"]
            hz = settings.get("fixed_timestep_hz", 60)
            # Check guards against two real relocated distributions, without relying
            # on the recorder to have gained the fields yet (red test on older players).
            bound = root / "Bound route.json"
            bound.write_text(json.dumps({**captured, "scene": startup, "fixed_hz": hz}), encoding="utf-8")
            alternate_scene = "Scenes/Alternate Proof.ascene"
            shutil.copyfile(hidden_project.parent / "Content" / startup,
                            hidden_project.parent / "Content" / alternate_scene)
            observed = []
            for edit, profile, message in (
                    ({"startup_scene": alternate_scene}, "Wrong Scene", "scene mismatch"),
                    ({"fixed_timestep_hz": hz * 2}, "Wrong Rate", "fixed tick rate mismatch")):
                mismatched = root / (profile + " Distribution")
                hidden_project.write_text(json.dumps({**settings, **edit}), encoding="utf-8")
                try:
                    run(tools["aether_package"], hidden_project, "--out", mismatched, *release_config, cwd=root)
                finally:
                    hidden_project.write_bytes(project_bytes)
                mismatch = subprocess.run([mismatched / executable, "--jump-proof", bound, "--jump-trace",
                                           "--user-dir", root / (profile + " Data")],
                                          cwd=empty_cwd, capture_output=True, text=True, timeout=30)
                combined = mismatch.stdout + mismatch.stderr
                if mismatch.returncode != 2 or message not in combined or "Jump Proof: " in combined or "Jump Trace: " in combined:
                    samples = [line for line in combined.splitlines() if not line.startswith("Jump Trace: ")]
                    observed.append((profile, mismatch.returncode, "\n".join(samples)[-1200:]))
            if observed:
                raise RuntimeError(f"bound route ran in a different environment: {observed}")
            if captured.get("scene") != startup or captured.get("fixed_hz") != hz:
                raise RuntimeError(f"route capture lost scene or fixed tick rate: {captured}")
            explicit = "./" + startup.replace("/", "\\")
            run(game, "--scene", explicit, "--jump-proof", recorded,
                "--user-dir", root / "Explicit Scene Data", cwd=empty_cwd)
            partial = root / "Partial route.json"
            partial.write_text(json.dumps({**spec, "scene": startup}), encoding="utf-8")
            rejected = subprocess.run([game, "--jump-proof", partial, "--user-dir", root / "Partial Route Data"],
                                      cwd=empty_cwd, capture_output=True, text=True, timeout=30)
            if (rejected.returncode != 2 or "binding" not in rejected.stderr or "Jump Proof: " in rejected.stdout):
                raise RuntimeError("incomplete route binding was not rejected: " + rejected.stderr)
            original_recording = recorded.read_bytes()
            duplicate = subprocess.run([game, "--headless", "--replay-input", route, "--record-jump-route", recorded,
                                        "--user-dir", root / "Duplicate Route Data"], cwd=empty_cwd,
                                       capture_output=True, text=True, timeout=30)
            if duplicate.returncode != 2 or recorded.read_bytes() != original_recording:
                raise RuntimeError("route capture overwrote or accepted a pre-existing recording")

            original_trace, changed_trace = root / "Before.txt", root / "After.txt"
            before = proof(game, 0, "Baseline Route Data", original_trace)
            if len(before) != 1 or before[0]["status"] != "pass" or before[0]["frames"] != 400:
                raise RuntimeError(f"baseline did not pass Jump Proof: {before}")
            hidden = root / "Hidden Project"
            tilemap = hidden / "Content/Tiles/Level1.atilemap"
            tiles = json.loads(tilemap.read_text(encoding="utf-8"))
            tile = 4 * tiles["size"][0] + 23
            if tiles["layers"][0]["cells"][tile] != -1:
                raise RuntimeError("Jump Proof regression tile is no longer empty")
            tiles["layers"][0]["cells"][tile] = -2  # make one new solid ground cell
            tilemap.write_text(json.dumps(tiles), encoding="utf-8")
            changed = root / "Changed Distribution"
            run(tools["aether_package"], hidden / project.name, "--out", changed, *release_config, cwd=root)
            after = proof(changed / executable, 5, "Changed Route Data", changed_trace)
            if (len(after) != 1 or after[0]["status"] != "missed" or after[0]["frames"] != 400
                    or after[0]["position"][0] >= 28.5):
                raise RuntimeError(f"single tile change did not fail Jump Proof: {after}")

            compare = Path(__file__).resolve().parents[1] / "jump_proof_compare.py"

            def compare_traces(left, right, code, *extra):
                result = subprocess.run([sys.executable, compare, left, right, *extra], cwd=empty_cwd,
                                        capture_output=True, text=True, timeout=10)
                if result.returncode != code:
                    raise RuntimeError(f"trace comparison exit={result.returncode}: {result.stdout}{result.stderr}")
                return json.loads(result.stdout) if code != 2 else None

            # A loose positional tolerance must not hide the changed route outcome.
            outcome = compare_traces(original_trace, changed_trace, 5, "--tolerance", "1000", "--overlay-data")
            if (outcome["first_difference_frame"] is not None or outcome.get("before_status") != "pass"
                    or outcome.get("after_status") != "missed" or outcome.get("verdict_changed") is not True
                    or len(outcome["before_path"]) != 400 or len(outcome["after_path"]) != 400):
                raise RuntimeError(f"target outcome did not survive positional tolerance: {outcome}")
            matching = compare_traces(original_trace, original_trace, 0)
            if (matching["first_difference_frame"] is not None or matching["frames"] != 400
                    or matching.get("route_verified") is not True or matching.get("before_status") != "pass"
                    or matching.get("after_status") != "pass" or matching.get("verdict_changed") is not False):
                raise RuntimeError(f"equal traces were not reported equal: {matching}")
            still_missed = compare_traces(changed_trace, changed_trace, 0)
            if still_missed.get("before_status") != "missed" or still_missed.get("verdict_changed") is not False:
                raise RuntimeError(f"matching misses were not reported as unchanged: {still_missed}")
            diff = compare_traces(original_trace, changed_trace, 5)
            if not 280 <= diff["first_difference_frame"] <= 300 or diff["frames"] != 400:
                raise RuntimeError(f"did not locate the first divergence after placing one tile: {diff}")
            overlay = compare_traces(original_trace, changed_trace, 5, "--overlay-data")
            i = overlay["first_difference_frame"] - 1
            if (len(overlay["before_path"]) != 400 or len(overlay["after_path"]) != 400
                    or overlay["before_path"][i] != diff["before"] or overlay["after_path"][i] != diff["after"]):
                raise RuntimeError("editor overlay data did not match the full paths and divergence")
            plain_file = root / "Replay Overlay café.json"
            compare_file = [sys.executable, compare, original_trace, changed_trace, "--overlay-data", "--output", plain_file]
            plain = subprocess.run(compare_file, cwd=empty_cwd, capture_output=True, text=True, timeout=10)
            if plain.returncode != 5 or plain.stdout or not plain_file.is_file():
                raise RuntimeError("ordinary overlay file export failed: " + plain.stdout + plain.stderr)
            plain_bytes = plain_file.read_bytes()
            if json.loads(plain_bytes.decode("utf-8")) != overlay:
                raise RuntimeError("ordinary overlay differed from stdout JSON")
            existing_plain = subprocess.run(compare_file, cwd=empty_cwd, capture_output=True, text=True, timeout=10)
            if (existing_plain.returncode != 2 or "already exists" not in existing_plain.stderr
                    or plain_file.read_bytes() != plain_bytes):
                raise RuntimeError("existing ordinary overlay was not protected: " + existing_plain.stderr)
            isolate = Path(__file__).resolve().parents[1] / "jump_proof_isolate_tile.py"
            operator = [sys.executable, isolate, "--pak-tool", tools["aether_pak"],
                        "--player", executable, "--route", route, relocated, changed]
            result = subprocess.run(operator, cwd=empty_cwd, capture_output=True, text=True, timeout=40)
            if result.returncode != 0:
                raise RuntimeError("controlled tile attribution failed: " + result.stdout + result.stderr)
            receipt = json.loads(result.stdout)
            if (receipt["cell"] != {"layer": 0, "x": 23, "y": 4, "before": -1, "after": -2}
                    or receipt["asset"] != "Content/Tiles/Level1.atilemap"
                    or receipt["first_difference_frame"] != diff["first_difference_frame"]):
                raise RuntimeError(f"wrong controlled attribution: {receipt}")
            overlay_file = root / "Jump Proof Overlay.json"
            export_cmd = [*operator, "--overlay-data", "--output", overlay_file]
            export = subprocess.run(export_cmd, cwd=empty_cwd, capture_output=True, text=True, timeout=40)
            if export.returncode != 0 or export.stdout or not overlay_file.is_file():
                raise RuntimeError("controlled overlay file export failed: " + export.stdout + export.stderr)
            original_bytes = overlay_file.read_bytes()
            confirmed_overlay = json.loads(original_bytes.decode("utf-8"))
            existing = subprocess.run(export_cmd, cwd=empty_cwd, capture_output=True, text=True, timeout=40)
            if existing.returncode != 2 or "already exists" not in existing.stderr or overlay_file.read_bytes() != original_bytes:
                raise RuntimeError("existing overlay was not protected: " + existing.stdout + existing.stderr)
            if (confirmed_overlay["cell"] != receipt["cell"] or confirmed_overlay["asset"] != receipt["asset"]
                    or confirmed_overlay["before_path"] != overlay["before_path"]
                    or confirmed_overlay["after_path"] != overlay["after_path"]):
                raise RuntimeError("controlled overlay lost its evidence")
            extra = changed / "uncontrolled.txt"
            extra.write_text("not a tile edit", encoding="utf-8")
            uncontrolled = subprocess.run(operator, cwd=empty_cwd, capture_output=True, text=True, timeout=40)
            if uncontrolled.returncode != 2 or "outside" not in uncontrolled.stderr:
                raise RuntimeError("the tool claimed attribution despite an uncontrolled change")
            extra.unlink()
            mismatched_route = root / "Different route.txt"
            original_lines = original_trace.read_text(encoding="utf-8").splitlines()
            metadata = [line for line in original_lines if line.startswith("Jump Route: ")]
            if len(metadata) != 1:
                raise RuntimeError("trace did not include exactly one route descriptor")
            for field in ("fixed_hz", "events", "target"):
                different = json.loads(metadata[0].removeprefix("Jump Route: "))
                if field == "fixed_hz":
                    different["fixed_hz"] *= 2
                elif field == "events":
                    different["events"][0]["frame"] += 1
                else:
                    different["target"]["max_x"] += 0.1
                mismatched_route.write_text("\n".join("Jump Route: " + json.dumps(different)
                                                       if line == metadata[0] else line for line in original_lines),
                                            encoding="utf-8")
                compare_traces(original_trace, mismatched_route, 2)
            shell_trace = root / "Shell capture.txt"
            captured_text = "\r\n".join(original_lines) + "\r\n"
            for bom, encoding in ((codecs.BOM_UTF8, "utf-8"), (codecs.BOM_UTF16_LE, "utf-16-le"),
                                  (codecs.BOM_UTF16_BE, "utf-16-be")):
                shell_trace.write_bytes(bom + captured_text.encode(encoding))
                converted = compare_traces(original_trace, shell_trace, 0)
                if converted != matching:
                    raise RuntimeError(f"{encoding} shell capture lost route evidence: {converted}")
            shell_trace.write_bytes(codecs.BOM_UTF16_LE + captured_text.encode("utf-16-le")[:-1])
            compare_traces(original_trace, shell_trace, 2)
            legacy = root / "Legacy.txt"
            legacy.write_text("\n".join(line for line in original_lines if line != metadata[0]), encoding="utf-8")
            if compare_traces(legacy, legacy, 0).get("route_verified") is not False:
                raise RuntimeError("old logs silently claimed route verification")
            compare_traces(original_trace, legacy, 2)
            # An intact trace with the wrong status is not evidence and must not export an overlay.
            inconsistent = root / "Inconsistent Verdict.txt"
            rejected_overlay = root / "Inconsistent Overlay.json"
            for source, status in ((original_trace, "missed"), (changed_trace, "pass")):
                verdict_prefix = "Jump Proof: "
                lines = source.read_text(encoding="utf-8").splitlines()
                if sum(line.startswith(verdict_prefix) for line in lines) != 1:
                    raise RuntimeError("trace did not include exactly one verdict")
                inconsistent.write_text("\n".join(
                    verdict_prefix + json.dumps({**json.loads(line.removeprefix(verdict_prefix)), "status": status})
                    if line.startswith(verdict_prefix) else line for line in lines), encoding="utf-8")
                result = subprocess.run([sys.executable, compare, inconsistent, inconsistent,
                                         "--overlay-data", "--output", rejected_overlay],
                                        cwd=empty_cwd, capture_output=True, text=True, timeout=10)
                if (result.returncode != 2 or "verdict disagrees with the route target" not in result.stderr
                        or rejected_overlay.exists()):
                    raise RuntimeError("inconsistent verdict was accepted: " + result.stdout + result.stderr)
            truncated = root / "Truncated.txt"
            truncated.write_text("\n".join(original_trace.read_text(encoding="utf-8").splitlines()[:-1]), encoding="utf-8")
            compare_traces(original_trace, truncated, 2)
            invalid_file = root / "Incomplete Overlay.json"
            compare_traces(original_trace, truncated, 2, "--overlay-data", "--output", invalid_file)
            if invalid_file.exists():
                raise RuntimeError("an incomplete trace wrote an overlay file")

            spec["target"]["min_x"] = 31
            route.write_text(json.dumps(spec), encoding="utf-8")
            if proof(game, 2, "Invalid Route Data"):
                raise RuntimeError("invalid target must not issue a route verdict")


if __name__ == "__main__":
    main()
