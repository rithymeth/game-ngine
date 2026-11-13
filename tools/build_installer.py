#!/usr/bin/env python3
"""Build the Windows application installer for a tagged release.

Configures a Release tree (the Dear ImGui editor, aether_editor_shell, with the Vulkan backend it draws through, and the dev tools, no tests), installs it to a staging prefix, zips the result and
compiles a self-extracting installer with the .NET Framework compiler that ships with Windows. The payload is embedded as a
managed resource, so the output is a single exe a user runs with no administrator rights. Run with no arguments it shows a setup
window (welcome, folder and options, progress, finish); with /silent it installs with no window and writes to the console. The installer's source is
tools/installer/AetherInstaller.cs: it closes (with the user's consent) any Aether program running from the install folder, moves
a previous installation aside (leaving it untouched if it is in use), checks that bin\aether_editor_shell.exe is there after unpacking, makes the Start menu and
Desktop shortcuts, and offers to start the editor. CI compiles and runs it on Windows (tools/installer/test_installer.ps1).

    python tools/build_installer.py --version 0.29.0

It works from a plain prompt (CMake then uses the installed Visual Studio) and from a Visual Studio developer prompt (Ninja, as CI does).

The result goes to dist/Aether-<version>-Windows-x64.exe.
"""

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
CSC = pathlib.Path(r"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe")

SOURCE = ROOT / "tools" / "installer" / "AetherInstaller.cs"


def run(command, **kwargs):
    result = subprocess.run(command, **kwargs)
    if result.returncode != 0:
        raise SystemExit(f"failed: {' '.join(str(c) for c in command)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="release version, for example 0.29.0")
    parser.add_argument("--keep", action="store_true", help="keep the temporary tree for inspection")
    args = parser.parse_args()

    work = pathlib.Path(tempfile.mkdtemp(prefix="aether-installer-"))
    tree = work / "build"
    stage = work / "stage"
    try:
        # Ninja needs a compiler on the PATH (a Visual Studio developer prompt, or CI's set-up step). From a plain prompt there is none, so let
        # CMake choose the installed Visual Studio instead, which finds its own compiler.
        generator = ["-G", "Ninja"] if shutil.which("cl") and shutil.which("ninja") else (["-A", "x64"] if sys.platform == "win32" else [])
        run(["cmake", "-S", str(ROOT), "-B", str(tree), *generator,
             "-DCMAKE_BUILD_TYPE=Release", "-DAETHER_BUILD_EDITOR=ON", "-DAETHER_BUILD_VULKAN=ON",
             "-DAETHER_BUILD_DEVTOOLS=ON", "-DAETHER_BUILD_BENCH=OFF",
             "-DAETHER_BUILD_FUZZERS=OFF", "-DAETHER_BUILD_TESTS=OFF"],
            stdout=subprocess.DEVNULL)
        run(["cmake", "--build", str(tree), "--config", "Release", "--parallel", "2"], stdout=subprocess.DEVNULL)
        run(["cmake", "--install", str(tree), "--prefix", str(stage), "--config", "Release"],
            stdout=subprocess.DEVNULL)

        payload = work / "aether-app.zip"
        with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(stage).as_posix())

        source = work / "AetherInstaller.cs"
        source.write_text(SOURCE.read_text(encoding="utf-8").replace("@VERSION@", args.version), encoding="utf-8")
        dist = ROOT / "dist"
        dist.mkdir(exist_ok=True)
        output = dist / f"Aether-{args.version}-Windows-x64.exe"
        run([str(CSC), "/nologo", "/target:winexe", "/optimize+", f"/out:{output}",
             f"/resource:{payload},aether-app.zip",
             "/reference:System.IO.Compression.FileSystem.dll", "/reference:System.IO.Compression.dll",
             "/reference:System.Windows.Forms.dll", "/reference:System.Drawing.dll", str(source)])
        print(f"wrote {output} ({output.stat().st_size / 1024 / 1024:.1f} MB)")
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())