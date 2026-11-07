#!/usr/bin/env python3
"""Build the Windows SDK installer for a tagged release.

Configures a lean Release tree (no Vulkan, no editor, no tests), installs it to a staging prefix, zips the result and
compiles a self-extracting installer with the .NET Framework compiler that ships with Windows. The payload is embedded as a
managed resource, so the output is a single exe a user runs with no administrator rights.

    python tools/build_sdk_installer.py --version 0.29.0

The result goes to dist/Aether-<version>-SDK-installer.exe.
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

PROGRAM = r"""
using System;
using System.IO;
using System.IO.Compression;
using System.Reflection;

class AetherInstaller
{
    static int Main(string[] args)
    {
        string target = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Aether");
        Console.WriteLine("Aether {version} SDK installer");
        Console.WriteLine("Installing to " + target);

        Stream payload = Assembly.GetExecutingAssembly().GetManifestResourceStream("aether-sdk.zip");
        if (payload == null) {{ Console.Error.WriteLine("installer payload missing"); return 2; }}

        string temp = Path.Combine(Path.GetTempPath(), "aether-install-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        string zipPath = Path.Combine(temp, "aether-sdk.zip");
        using (var file = File.Create(zipPath)) {{ payload.CopyTo(file); }}

        try
        {{
            if (Directory.Exists(target)) Directory.Delete(target, true);
            Directory.CreateDirectory(target);
            ZipFile.ExtractToDirectory(zipPath, target);
        }}
        catch (Exception error)
        {{
            Console.Error.WriteLine("install failed: " + error.Message);
            return 1;
        }}
        finally {{ try {{ Directory.Delete(temp, true); }} catch {{ }} }}

        Console.WriteLine();
        Console.WriteLine("Installed " + target);
        Console.WriteLine();
        Console.WriteLine("To use it from CMake:");
        Console.WriteLine("  cmake -S . -B build -DCMAKE_PREFIX_PATH=\\"" + target + "\\"");
        Console.WriteLine("  find_package(Aether REQUIRED)");
        Console.WriteLine("  target_link_libraries(app PRIVATE Aether::Engine)");
        return 0;
    }}
}}
"""


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
        run(["cmake", "-S", str(ROOT), "-B", str(tree), "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Release", "-DAETHER_BUILD_VULKAN=OFF",
             "-DAETHER_BUILD_EDITOR=OFF", "-DAETHER_BUILD_EDITOR_UI=OFF",
             "-DAETHER_BUILD_BENCH=OFF", "-DAETHER_BUILD_FUZZERS=OFF",
             "-DAETHER_BUILD_TESTS=OFF", "-DAETHER_BUILD_DEVTOOLS=OFF"],
            stdout=subprocess.DEVNULL)
        run(["cmake", "--build", str(tree), "--parallel", "2"], stdout=subprocess.DEVNULL)
        run(["cmake", "--install", str(tree), "--prefix", str(stage), "--config", "Release"],
            stdout=subprocess.DEVNULL)

        payload = work / "aether-sdk.zip"
        with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(stage))

        source = work / "AetherInstaller.cs"
        source.write_text(PROGRAM.format(version=args.version), encoding="utf-8")
        dist = ROOT / "dist"
        dist.mkdir(exist_ok=True)
        output = dist / f"Aether-{args.version}-SDK-installer.exe"
        run([str(CSC), "/nologo", "/target:winexe", "/optimize+", f"/out:{output}",
             f"/resource:{payload},aether-sdk.zip",
             "/reference:System.IO.Compression.FileSystem.dll", str(source)])
        print(f"wrote {output} ({output.stat().st_size / 1024 / 1024:.1f} MB)")
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())