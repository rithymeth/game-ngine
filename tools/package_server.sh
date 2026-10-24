#!/usr/bin/env bash
# M8.9: packages the dedicated server - aether_server, aether_netclient, a
# server.json and a cooked game's .apak archives - into
#   <out>/aether-server-<platform>.tar.gz   (Linux, macOS; one top-level folder of the same name)
#   <out>/aether-server-<platform>.zip      (Windows)
#   <out>/SHA256SUMS.txt                    (sha256sum format, like the other release artifacts)
#   <out>/SERVER_PACKAGE.json               (name, bytes, sha256 of the archive and every file in it)
#
#   tools/package_server.sh --build <build dir> --paks <cooked dir> --out <dir>
#                           [--platform linux-x64|macos-arm64|windows-x64] [--config <server.json>] [--no-strip]
#
# The cooked dir is aether_cook's --out (its *.apak are copied to Paks/). The
# package runs from anywhere: bin/aether_server finds ../server.json and ../Paks.
# Runs under bash on Linux, macOS (bash 3.2) and Windows (Git Bash); the
# archive is written by Python (tarfile/zipfile) with sorted names, a fixed
# owner and mtime, so it is reproducible on every platform. The binaries are
# found in <build>/tools/server/ or, for multi-config generators (MSVC),
# <build>/tools/server/Release/.
set -euo pipefail

build="" paks="" out="" platform="" config="" strip_bins=1
while [ $# -gt 0 ]; do
    case "$1" in
        --build) build="$2"; shift 2 ;;
        --paks) paks="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --platform) platform="$2"; shift 2 ;;
        --config) config="$2"; shift 2 ;;
        --no-strip) strip_bins=0; shift ;;
        *) echo "package_server.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$build" ] && [ -n "$paks" ] && [ -n "$out" ] || { sed -n '2,20p' "$0" >&2; exit 2; }
here="$(cd "$(dirname "$0")" && pwd)"
[ -n "$config" ] || config="$here/server/server.json"
if [ -z "$platform" ]; then
    case "$(uname -s)" in
        Darwin) platform="macos-$(uname -m | sed 's/x86_64/x64/')" ;;
        MINGW*|MSYS*|CYGWIN*) platform="windows-x64" ;;
        *) platform="linux-$(uname -m | sed 's/x86_64/x64/;s/aarch64/arm64/')" ;;
    esac
fi
case "$platform" in windows-*) exe=".exe" format="zip" ;; *) exe="" format="tar.gz" ;; esac
# python3 first; on Windows "python3" can be the Store stub, so check that it runs.
py=""
for c in python3 python; do if "$c" -c "" > /dev/null 2>&1; then py="$c"; break; fi; done
[ -n "$py" ] || { echo "package_server.sh: needs python3" >&2; exit 1; }

bindir=""
for d in "$build/tools/server/Release" "$build/tools/server"; do
    if [ -f "$d/aether_server$exe" ] && [ -f "$d/aether_netclient$exe" ]; then bindir="$d"; break; fi
done
[ -n "$bindir" ] || { echo "package_server.sh: aether_server$exe / aether_netclient$exe not in $build/tools/server[/Release] (build them first)" >&2; exit 1; }
shopt -s nullglob
apaks=("$paks"/*.apak)
[ ${#apaks[@]} -gt 0 ] || { echo "package_server.sh: no .apak in $paks" >&2; exit 1; }

name="aether-server-$platform"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
stage="$out/$name"
rm -rf "$stage" "$out/$name.tar.gz" "$out/$name.zip"
mkdir -p "$stage/bin" "$stage/Paks" "$stage/logs"
cp "$bindir/aether_server$exe" "$bindir/aether_netclient$exe" "$stage/bin/"
# Strip ELF/Mach-O symbols (-x keeps what dyld needs on macOS); MSVC keeps its symbols in .pdb files already.
if [ "$strip_bins" = 1 ] && [ -z "$exe" ] && command -v strip > /dev/null; then
    case "$platform" in macos-*) strip -x "$stage/bin/aether_server" "$stage/bin/aether_netclient" ;; *) strip "$stage/bin/aether_server" "$stage/bin/aether_netclient" ;; esac
fi
cp "${apaks[@]}" "$stage/Paks/"
cp "$config" "$stage/server.json"
cp "$here/server/README.txt" "$stage/README.txt"
: > "$stage/logs/.keep" # zip drops empty folders

mtime="${SOURCE_DATE_EPOCH:-$(git -C "$here/.." log -1 --format=%ct 2>/dev/null || echo 0)}"
"$py" - "$out" "$name" "$format" "$mtime" <<'PY'
import gzip, hashlib, io, json, os, sys, tarfile, time, zipfile
out, name, fmt, mtime = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
stage = os.path.join(out, name)
def digest(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()
entries = []  # (archive path, file path or None for a folder)
for root, dirs, names in os.walk(stage):
    dirs.sort()
    rel = os.path.relpath(root, out).replace(os.sep, "/")
    entries.append((rel + "/", None))
    for n in sorted(names): entries.append((rel + "/" + n, os.path.join(root, n)))
entries.sort()
def executable(arc): return "/bin/" in arc
archive = os.path.join(out, name + "." + fmt)
if fmt == "zip":
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        stamp = max((1980, 1, 1, 0, 0, 0), tuple(time.gmtime(mtime)[:6]))
        for arc, path in entries:
            info = zipfile.ZipInfo(arc, stamp)
            info.external_attr = ((0o40755 if path is None else (0o100755 if executable(arc) else 0o100644)) << 16) | (0x10 if path is None else 0)
            info.compress_type = zipfile.ZIP_STORED if path is None else zipfile.ZIP_DEFLATED
            z.writestr(info, b"" if path is None else open(path, "rb").read())
else:
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as t:
        for arc, path in entries:
            info = tarfile.TarInfo(arc.rstrip("/"))
            info.mtime, info.uid, info.gid, info.uname, info.gname = mtime, 0, 0, "", ""
            if path is None:
                info.type, info.mode = tarfile.DIRTYPE, 0o755
                t.addfile(info)
            else:
                info.size, info.mode = os.path.getsize(path), 0o755 if executable(arc) else 0o644
                with open(path, "rb") as f: t.addfile(info, f)
    with open(archive, "wb") as f, gzip.GzipFile(filename="", mode="wb", fileobj=f, compresslevel=9, mtime=0) as g:
        g.write(raw.getvalue())
files = [{"path": os.path.relpath(p, stage).replace(os.sep, "/"), "bytes": os.path.getsize(p), "sha256": digest(p)}
         for arc, p in entries if p is not None]
sha = digest(archive)
with open(os.path.join(out, "SHA256SUMS.txt"), "w", newline="\n") as f: f.write(f"{sha}  {os.path.basename(archive)}\n")
manifest = {"name": os.path.basename(archive), "platform": name[len("aether-server-"):], "bytes": os.path.getsize(archive),
            "sha256": sha, "files": sorted(files, key=lambda f: f["path"])}
with open(os.path.join(out, "SERVER_PACKAGE.json"), "w", newline="\n") as f: json.dump(manifest, f, indent=2)
PY
rm -rf "$stage"
cat "$out/SHA256SUMS.txt"
