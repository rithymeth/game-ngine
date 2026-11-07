#!/usr/bin/env python3
"""Build the Windows SDK installer for a tagged release.

Configures a lean Release tree (no Vulkan, no editor, no tests), installs it to a staging prefix, zips the result and
compiles a self-extracting installer with the .NET Framework compiler that ships with Windows. The payload is embedded as a
managed resource, so the output is a single exe a user runs with no administrator rights.

    python tools/build_installer.py --version 0.29.0

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

PROGRAM = r"""
using System;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Text;

class AetherInstaller
{{
    static readonly string LogPath = Path.Combine(Path.GetTempPath(), "aether-install-log.txt");
    static readonly StringBuilder Log = new StringBuilder();

    static void Note(string line) {{ Console.WriteLine(line); Log.AppendLine(line); }}

    static void Pause()
    {{
        Console.WriteLine();
        Console.Write("Press Enter to close...");
        try {{ Console.ReadLine(); }} catch {{ }}
    }}

    static void WriteLog() {{ try {{ File.WriteAllText(LogPath, Log.ToString()); }} catch {{ }} }}

    static int Fail(string message)
    {{
        Console.Error.WriteLine();
        Console.Error.WriteLine("INSTALL FAILED: " + message);
        Console.Error.WriteLine("Log written to " + LogPath);
        Log.AppendLine("FAILED: " + message);
        WriteLog();
        Pause();
        return 1;
    }}

    static int Main(string[] args)
    {{
        try
        {{
            Note("Aether Engine {version} - full application installer");
            Note("");
            Stream payload = Assembly.GetExecutingAssembly().GetManifestResourceStream("aether-app.zip");
            if (payload == null) return Fail("the payload is not inside this executable");
            Note("Payload found: " + (payload.Length / 1024 / 1024) + " MB");

            string target = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Aether");
            Note("Target: " + target);

            string temp = Path.Combine(Path.GetTempPath(), "aether-install-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(temp);
            string zipPath = Path.Combine(temp, "aether-app.zip");
            Note("Writing the payload to a scratch folder...");
            using (FileStream file = File.Create(zipPath)) {{ payload.CopyTo(file); }}

            if (Directory.Exists(target))
            {{
                Note("Removing the previous installation...");
                Directory.Delete(target, true);
            }}
            Directory.CreateDirectory(target);

            Note("Unpacking the SDK, this takes a few moments...");
            ZipFile.ExtractToDirectory(zipPath, target);
            try {{ Directory.Delete(temp, true); }} catch {{ }}

            string[] files = Directory.GetFiles(target, "*", SearchOption.AllDirectories);
            Note("");
            Note("Installed " + files.Length + " files to " + target);
            Note("");
            Note("To use it from CMake:");
            Note("  cmake -S . -B build -DCMAKE_PREFIX_PATH=\\"" + target + "\\"");
            Note("  find_package(Aether REQUIRED)");
            Note("  target_link_libraries(app PRIVATE Aether::Engine)");
            WriteLog();
            Pause();
            return 0;
        }}
        catch (Exception error)
        {{
            return Fail(error.GetType().Name + ": " + error.Message);
        }}
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
             "-DCMAKE_BUILD_TYPE=Release", "-DAETHER_BUILD_EDITOR=ON",
             "-DAETHER_BUILD_DEVTOOLS=ON", "-DAETHER_BUILD_BENCH=OFF",
             "-DAETHER_BUILD_FUZZERS=OFF", "-DAETHER_BUILD_TESTS=OFF"],
            stdout=subprocess.DEVNULL)
        run(["cmake", "--build", str(tree), "--parallel", "2"], stdout=subprocess.DEVNULL)
        run(["cmake", "--install", str(tree), "--prefix", str(stage), "--config", "Release"],
            stdout=subprocess.DEVNULL)

        payload = work / "aether-app.zip"
        with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(stage))

        source = work / "AetherInstaller.cs"
        source.write_text(PROGRAM.format(version=args.version), encoding="utf-8")
        dist = ROOT / "dist"
        dist.mkdir(exist_ok=True)
        output = dist / f"Aether-{args.version}-Windows-x64.exe"
        run([str(CSC), "/nologo", "/target:exe", "/optimize+", f"/out:{output}",
             f"/resource:{payload},aether-app.zip",
             "/reference:System.IO.Compression.FileSystem.dll", str(source)])
        print(f"wrote {output} ({output.stat().st_size / 1024 / 1024:.1f} MB)")
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())